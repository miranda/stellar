-- stellar_menu_behavior.lua
-- Add-on behaviors for awful.menu, applied by wrapping a freshly built menu:
--
--   1. Corner-cut ("triangle") tolerance, menu-aim style. While a submenu is
--      open, hovering a sibling row is allowed through INSTANTLY unless the
--      pointer's recent motion is genuinely aimed into the submenu's near
--      edge. Aimed motion is held -- but every hold carries a short grace
--      timer that re-evaluates and then COMMITS the switch, so the menu can
--      never latch: the worst case is a ~GRACE_DELAY pause, after which the
--      hovered row wins. Works whether the submenu opened to the right or
--      (near the screen edge) to the left.
--
--   2. Auto-hide: the whole menu tree closes after AUTOHIDE_SECONDS with the
--      pointer outside every visible menu. The timer resets whenever the
--      pointer is over any menu in the tree.
--
-- WHY A WRAPPER, NOT A PATCH:
-- awful.menu has no hover-delay or triangle setting to override -- submenu
-- show/hide is hardwired into its item_enter. So we don't configure it; we
-- intercept those hooks after the menu object exists. apply(menu) is called
-- once per rebuild(), re-arming behavior on every fresh menu.
--
-- HOW THE TRIANGLE DECISION WORKS (and why it's shaped this way):
-- A background sampler records the pointer position every SAMPLE_INTERVAL
-- while the menu is visible, so we always have a *fresh* motion vector.
-- (The old version derived direction from the previous enter/leave event,
-- which after any excursion into a submenu was stale garbage -- one pixel of
-- rightward jitter during vertical browsing read as "heading for the
-- submenu" and locked the selection.)
--
-- On a hovered sibling we build the classic menu-aim triangle: apex at the
-- pointer position ~HIST_N samples ago, base spanning the open submenu's
-- NEAR edge (the edge adjacent to this menu, padded by TRIANGLE_SLACK).
-- Pointer inside that triangle and moving = aimed = hold. Anything else --
-- vertical browsing, moving away, or a pointer that has effectively stopped
-- (< MIN_MOVE px) -- switches immediately. The near edge is chosen per side,
-- so left-opening submenus (root close to the right screen edge) get exactly
-- the same protection as right-opening ones.
--
-- A hold is never silent-forever: it arms a pending switch that fires after
-- GRACE_DELAY. At fire time we look again: pointer made it into the submenu
-- -> hold succeeded, forget it; pointer still aiming -> extend (up to
-- MAX_HOLD total); otherwise -> commit the switch to whatever row the
-- pointer is on. New rows entered while pending simply retarget the same
-- pending switch. The old design's twin failure modes -- swallowing the
-- enter with no timer (stuck until the pointer re-crossed a row boundary)
-- and infinite holds -- are both structurally impossible here.
--
--- This reaches into awful.menu internals:
--- (item_enter, exec, .child, .active_child, per-menu .wibox geometry).
--   * hover enters arrive as item_enter(num, {hover=true, mouse=true})
--   * keyboard/click enters carry no .hover -> they bypass all of this
--   * submenus are built lazily in exec() and cached in self.child[num]
--   * the open submenu is self.active_child, its drawin is .wibox

local gears = require("gears")
local mouse = mouse           -- AwesomeWM global
local mousegrabber = mousegrabber

local B = {}

-- Timers of the most recently armed menu. Because rebuild() creates a fresh
-- menu object on every MENU_DATA push, we stop the previous menu's timers
-- when a new one is armed so orphaned pollers don't linger.
local _last_autohide_timer = nil
local _last_sampler        = nil

-- ---- Runtime trace logging (temporary) -----------------------------------
-- Writes to stellar_api.log if reachable, and ALWAYS appends to a file so it
-- cannot silently vanish. Throttled per-tag so a hot path (hover, poll) logs
-- at most once per THROTTLE seconds and doesn't flood.
local _trace_last = {}
local TRACE_THROTTLE = 3.0   -- near-silent; lower this if you need to debug again
local function trace(tag, msg)
    local now = os.clock()
    if _trace_last[tag] and (now - _trace_last[tag]) < TRACE_THROTTLE then
        return
    end
    _trace_last[tag] = now
    local line = string.format("[stellar_menu %s] %s", tag, tostring(msg))
    local api = rawget(_G, "stellar_api")
    if api and type(api.log) == "function" then pcall(api.log, line) end
    local ok, f = pcall(io.open, "/tmp/stellar-menu-trace.log", "a")
    if ok and f then
        f:write(os.date("%H:%M:%S ") .. line .. "\n")
        f:close()
    end
end

-- ---- Tunables ------------------------------------------------------------
-- Triangle / hold behavior
local TRIANGLE_SLACK  = 8      -- px of vertical padding on the submenu's near
                               -- edge; raise to make corner-cutting more
                               -- forgiving, lower to make it stricter.
local GRACE_DELAY     = 0.20   -- s a held (aimed) hover waits before the
                               -- pending switch re-evaluates / commits. This
                               -- is the worst-case "lag" a wrongly-held row
                               -- can ever see.
local MAX_HOLD        = 1.50   -- s total a single aim may keep extending its
                               -- hold before the switch is forced through.
local SAMPLE_INTERVAL = 0.04   -- s between pointer samples while menu visible
local HIST_N          = 5      -- samples kept; the aim vector spans the oldest
                               -- -> now, i.e. ~HIST_N * SAMPLE_INTERVAL of motion
local MIN_MOVE        = 4      -- px the pointer must have moved over that span
                               -- for its direction to be trusted at all; below
                               -- this it's jitter and the switch is instant.
-- Auto-hide
local AUTOHIDE_SECONDS = 3.0   -- s outside the tree before it closes
local POLL_INTERVAL    = 0.20  -- s between pointer-location polls for autohide
-- Slack used when testing "is the pointer inside the menu tree" for autohide.
-- Kept modest so the countdown actually begins once the pointer leaves the
-- menu; too-large a value here means the pointer never reads as "outside".
local AUTOHIDE_SLACK   = 6     -- px
-- --------------------------------------------------------------------------

-- Is point (px,py) inside a wibox's rectangle, padded by `slack`?
local function in_wibox(wb, px, py, slack)
    if not wb then return false end
    -- A menu's drawin exposes x/y/width/height.
    local x, y, w, h = wb.x, wb.y, wb.width, wb.height
    if not (x and y and w and h) then return false end
    slack = slack or 0
    return px >= (x - slack) and px <= (x + w + slack)
       and py >= (y - slack) and py <= (y + h + slack)
end

-- Collect the wiboxes of every currently-visible menu in the tree, starting
-- from the root and walking .child links. Guarded so a missing .child or
-- .wibox just prunes that branch.
local function visible_wiboxes(root)
    local out = {}
    local seen = {}
    local function walk(m)
        if not m or seen[m] then return end
        seen[m] = true
        -- A menu exposes its drawin as .wibox on 4.x.
        if m.wibox and m.wibox.visible then
            out[#out + 1] = m.wibox
        end
        -- Open children live in .child (a map keyed by item index).
        if type(m.child) == "table" then
            for _, c in pairs(m.child) do walk(c) end
        end
    end
    walk(root)
    return out
end

-- Is the pointer over ANY visible menu in the tree (with slack)?
local function pointer_in_tree(root, slack)
    local c = mouse.coords()
    for _, wb in ipairs(visible_wiboxes(root)) do
        if in_wibox(wb, c.x, c.y, slack) then return true end
    end
    return false
end

-- ========================================================================
-- Triangle / corner-cut tolerance (menu-aim)
-- ========================================================================

-- Standard sign-based point-in-triangle test. Robust to vertex order and to
-- the degenerate triangles you get when menus overlap slightly.
local function in_triangle(px, py, ax, ay, bx, by, cx, cy)
    local function s(x1, y1, x2, y2, x3, y3)
        return (x1 - x3) * (y2 - y3) - (x2 - x3) * (y1 - y3)
    end
    local d1 = s(px, py, ax, ay, bx, by)
    local d2 = s(px, py, bx, by, cx, cy)
    local d3 = s(px, py, cx, cy, ax, ay)
    local has_neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    local has_pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    return not (has_neg and has_pos)
end

-- The submenu currently open off menu `m`: awful.menu sets m.active_child
-- and shows its .wibox.
local function active_sub_wibox(m)
    local ac = m.active_child
    if ac and ac.wibox and ac.wibox.visible then return ac.wibox end
    return nil
end

-- Arm the whole tree rooted at `root`. One shared state bundle carries the
-- pointer history, the tick clock, and the single pending switch; child
-- menus (created lazily by awful's exec) are wrapped into the same bundle
-- the moment they come into existence, so nested submenus behave too.
local function arm_triangle_tree(root)
    local S = {
        hist    = {},    -- oldest-first ring of {x,y} pointer samples
        ticks   = 0,     -- +1 per SAMPLE_INTERVAL while the root is visible.
                         -- This is our wall clock: os.clock() is CPU time and
                         -- barely advances in an event-driven WM, and gears
                         -- timers ARE wall-time, so counting their fires is
                         -- the reliable way to measure real elapsed time.
        pending = nil,   -- { menu=, num=, timer=, start_tick= } or nil
    }
    local MAX_HOLD_TICKS = math.max(1, math.ceil(MAX_HOLD / SAMPLE_INTERVAL))

    local function clear_pending()
        local p = S.pending
        S.pending = nil
        if p and p.timer then
            pcall(function() p.timer:stop() end)
        end
    end

    -- Perform the real switch: call the menu's ORIGINAL item_enter with the
    -- same opts a genuine hover would carry. sel was never touched during
    -- the hold, so awful's own sel==num early-return can't eat this.
    local function commit(m, num)
        local oe = m and m._stellar_orig_item_enter
        if not oe then return end
        trace("commit", "committing held switch to item " .. tostring(num))
        pcall(oe, m, num, { hover = true, mouse = true })
    end

    -- Is the pointer's recent motion aimed into `sub`'s near edge?
    local function aimed(sub)
        local P = S.hist[1]
        if not P then return false end        -- no history yet: don't hold
        local c = mouse.coords()
        local dx, dy = c.x - P.x, c.y - P.y
        if (dx * dx + dy * dy) < (MIN_MOVE * MIN_MOVE) then
            return false                      -- effectively parked: instant
        end
        -- Near edge = the submenu edge adjacent to where the pointer came
        -- from. Choosing it per side is what makes left-opening submenus
        -- (root near the right screen edge) work identically.
        local near_x = (sub.x >= P.x) and sub.x or (sub.x + sub.width)
        local top = sub.y - TRIANGLE_SLACK
        local bot = sub.y + sub.height + TRIANGLE_SLACK
        return in_triangle(c.x, c.y, P.x, P.y, near_x, top, near_x, bot)
    end

    -- Wrap one menu object; children wrap themselves on creation via the
    -- exec hook below. Idempotent per menu instance.
    local function wrap_menu(m)
        if not m or m._stellar_triangle_wrapped then return end
        if type(m.item_enter) ~= "function" then
            return  -- nothing to hook; feature absent, plain behavior stands
        end
        m._stellar_triangle_wrapped = true

        local orig_enter = m.item_enter
        m._stellar_orig_item_enter = orig_enter

        m.item_enter = function(self, num, opts)
            opts = opts or {}

            -- Keyboard / click / programmatic enters always pass straight
            -- through (and dissolve any pending hold -- the user acted).
            if not opts.hover then
                clear_pending()
                return orig_enter(self, num, opts)
            end

            local sub = active_sub_wibox(self)

            -- No submenu open, or re-hovering the row that owns it: nothing
            -- to protect. Instant, and any pending hold is moot.
            if not sub or self.sel == num then
                clear_pending()
                return orig_enter(self, num, opts)
            end

            -- A submenu is open and this is a DIFFERENT sibling. Hold only
            -- if the pointer is genuinely corner-cutting toward the submenu;
            -- vertical browsing, retreat, and jitter all switch instantly.
            if not aimed(sub) then
                clear_pending()
                return orig_enter(self, num, opts)
            end

            -- Aimed: hold. If a pending switch already exists for this menu,
            -- just retarget it (keeps the original hold budget so a slow
            -- diagonal can't extend itself forever); otherwise arm one.
            if S.pending and S.pending.menu == self then
                S.pending.num = num
                return
            end
            clear_pending()

            local p = { menu = self, num = num, start_tick = S.ticks }
            S.pending = p
            p.timer = gears.timer.start_new(GRACE_DELAY, function()
                -- Return true  -> timer restarts (keep holding).
                -- Return false -> timer stops (resolved one way or another).
                if S.pending ~= p then return false end   -- superseded

                local root_vis = self.wibox and self.wibox.visible
                if not root_vis then
                    clear_pending()
                    return false
                end

                local sub_now = active_sub_wibox(self)
                if not sub_now then
                    -- Submenu vanished while we held; just let the hovered
                    -- row win so the selection catches up.
                    local num_now = p.num
                    clear_pending()
                    commit(self, num_now)
                    return false
                end

                local c = mouse.coords()
                if in_wibox(sub_now, c.x, c.y, 2) then
                    clear_pending()          -- made it: the hold did its job
                    return false
                end
                if not in_wibox(self.wibox, c.x, c.y, 2) then
                    clear_pending()          -- wandered off entirely;
                    return false             -- autohide owns that case
                end

                -- Still on the parent. Keep holding only while the FRESH
                -- motion still aims at the submenu and the budget lasts.
                if aimed(sub_now)
                   and (S.ticks - p.start_tick) < MAX_HOLD_TICKS then
                    return true
                end

                local num_now = p.num
                clear_pending()
                commit(self, num_now)
                return false
            end)
            trace("hold", "holding item " .. tostring(num) ..
                          " (aimed at open submenu)")
            return
        end

        -- Children are built lazily inside awful's exec() and cached in
        -- self.child[num]. Hook exec so each child is wrapped into
        -- this same state bundle the moment it exists, giving nested
        -- submenus identical behavior.
        if type(m.exec) == "function" then
            local orig_exec = m.exec
            m.exec = function(self, num, ...)
                local r = orig_exec(self, num, ...)
                if type(self.child) == "table" and self.child[num] then
                    wrap_menu(self.child[num])
                end
                return r
            end
        end
    end

    -- Pointer sampler: the tree's motion memory and wall clock. Lives as
    -- long as the menu object; retired when the next rebuild arms a new one.
    if _last_sampler then
        _last_sampler:stop()
        _last_sampler = nil
    end
    local sampler = gears.timer({
        timeout   = SAMPLE_INTERVAL,
        autostart = true,          -- cheap when menu closed: one visible check
        call_now  = false,
        callback  = function()
            if not (root.wibox and root.wibox.visible) then
                if #S.hist > 0 then S.hist = {} end
                if S.pending then clear_pending() end
                return true        -- stay alive for the next open
            end
            S.ticks = S.ticks + 1
            local c = mouse.coords()
            S.hist[#S.hist + 1] = { x = c.x, y = c.y }
            if #S.hist > HIST_N then table.remove(S.hist, 1) end
            return true
        end,
    })
    root._stellar_sampler = sampler
    _last_sampler = sampler

    wrap_menu(root)
end

-- ========================================================================
-- Auto-hide after AUTOHIDE_SECONDS outside the tree
-- ========================================================================
local function arm_autohide(menu)
    if menu._stellar_autohide_wrapped then return end
    menu._stellar_autohide_wrapped = true

    -- Retire the previous menu's poller (rebuild made a new menu object).
    if _last_autohide_timer then
        _last_autohide_timer:stop()
        _last_autohide_timer = nil
    end

    -- Countdown by COUNTING TICKS, not by clock arithmetic. Critical bug avoided
    -- here: os.clock() returns CPU time, which barely advances in an event-driven
    -- WM that sleeps while idle -- so an os.clock()-based deadline effectively
    -- never elapses in real time. The timer fires every POLL_INTERVAL seconds of
    -- WALL time, so N ticks == N * POLL_INTERVAL real seconds. That's our clock.
    local ticks_to_hide = math.max(1, math.ceil(AUTOHIDE_SECONDS / POLL_INTERVAL))
    local ticks_outside = 0
    local SLACK = AUTOHIDE_SLACK

    -- One long-lived poller. It does NOT assume anything about how the menu was
    -- shown (toggle/show/keyboard all fine) -- each tick it reads live state:
    --   * root wibox not visible  -> menu is closed; reset the counter
    --   * pointer over any menu    -> reset the counter (still inside)
    --   * counter reaches limit    -> hide the whole tree
    local poll
    poll = gears.timer({
        timeout   = POLL_INTERVAL,
        autostart = true,          -- runs from creation; cheap when menu closed
        call_now  = false,
        callback  = function()
            local root_vis = menu.wibox and menu.wibox.visible
            if not root_vis then
                ticks_outside = 0     -- menu closed; nothing to count
                return true           -- keep timer alive for the next open
            end

            if pointer_in_tree(menu, SLACK) then
                ticks_outside = 0     -- inside: reset the countdown
            else
                ticks_outside = ticks_outside + 1
                if ticks_outside >= ticks_to_hide then
                    ticks_outside = 0
                    trace("hide", "autohide firing after " ..
                          tostring(AUTOHIDE_SECONDS) .. "s outside")
                    if menu.hide then pcall(function() menu:hide() end) end
                end
            end
            return true
        end,
    })

    -- Stash so a later rebuild's stale timer could be stopped if desired.
    menu._stellar_autohide_timer = poll
    _last_autohide_timer = poll
end

local _probed = false
local function probe(menu)
    if _probed then return end
    _probed = true

    local lines = {}
    local function say(s)
        lines[#lines + 1] = "[stellar_menu probe] " .. tostring(s)
    end

    say("apply ran; menu=" .. type(menu))
    say("item_enter=" .. type(menu.item_enter) .. " item_leave=" .. type(menu.item_leave))
    say("show=" .. type(menu.show) .. " hide=" .. type(menu.hide) ..
        " exec=" .. type(menu.exec))
    say("wibox=" .. tostring(menu.wibox) .. " child=" .. type(menu.child))
    if menu.wibox then
        say("wibox.visible=" .. tostring(menu.wibox.visible) ..
            " x=" .. tostring(menu.wibox.x) .. " w=" .. tostring(menu.wibox.width))
    end
    local keys = {}
    for k in pairs(menu) do keys[#keys + 1] = tostring(k) end
    table.sort(keys)
    say("keys: " .. table.concat(keys, ", "))

    -- Path 1: stellar_api.log, resolved from the global namespace at call time.
    local api = rawget(_G, "stellar_api")
    if api and type(api.log) == "function" then
        for _, l in ipairs(lines) do pcall(api.log, l) end
    end

    -- Path 2: unconditional file write, so the diagnostic can never disappear
    -- even if stellar_api isn't in scope here.
    local ok, f = pcall(io.open, "/tmp/stellar-menu-probe.log", "w")
    if ok and f then
        f:write(os.date("%H:%M:%S") .. " ---- probe ----\n")
        for _, l in ipairs(lines) do f:write(l .. "\n") end
        f:close()
    end
end

-- Public: apply all behaviors to a freshly built awful.menu. Idempotent-ish;
-- call once per rebuild on the new menu object.
function B.apply(menu)
    if not menu then return menu end
    probe(menu)
    arm_triangle_tree(menu)
    arm_autohide(menu)
    return menu
end

return B

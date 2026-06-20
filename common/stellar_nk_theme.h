// stellar_nk_theme.h

#ifndef STELLAR_NK_THEME_H
#define STELLAR_NK_THEME_H

// Forward declarations
struct nk_cairo_context;

// Cleans up theme resources and purges Cairo's XCB device cache.
// MUST be called before nk_cairo_free() for the context the font was created
// on, especially in apps that create/destroy X11 connections repeatedly.
void stellar_nk_theme_cleanup(struct nk_cairo_context *cairo_ctx);

#endif // STELLAR_NK_THEME_H

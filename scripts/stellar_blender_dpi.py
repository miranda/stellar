import bpy
import subprocess
import os

def sync_blender_dpi():
    # 1. Check for a custom environment variable override first
    env_scale = os.environ.get('STELLAR_UI_SCALE')
    if env_scale:
        try:
            bpy.context.preferences.view.ui_scale = float(env_scale)
            return
        except ValueError:
            pass
            
    # 2. Fall back to querying X11 for the system-wide Xft.dpi
    try:
        xrdb = subprocess.run(['xrdb', '-query'], capture_output=True, text=True)
        for line in xrdb.stdout.splitlines():
            if line.startswith('Xft.dpi:'):
                dpi = float(line.split(':')[1].strip())
                # Blender uses 72 DPI as its baseline for a 1.0 scale
                bpy.context.preferences.view.ui_scale = dpi / 72.0
                break
    except Exception as e:
        print(f"Failed to fetch X11 DPI: {e}")

sync_blender_dpi()

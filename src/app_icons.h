#ifndef WIN31X_APP_ICONS_H
#define WIN31X_APP_ICONS_H

/*
 * Resolve a freedesktop Desktop Entry Icon value to an existing raster file.
 *
 * icon_name may be an absolute filename or an icon name.  Named icons are
 * searched in hicolor application directories below the XDG data roots, in
 * XDG precedence order, and then in those roots' pixmaps directories.  The
 * legacy ~/.icons/hicolor directory is also searched.  Only regular, readable
 * PNG and XPM files are returned; SVG rendering belongs in a separate decoder.
 *
 * For a named icon, a native target_size candidate is preferred.  Otherwise
 * the nearest larger candidate is chosen, followed by the largest smaller
 * candidate.  PNG wins over XPM at the same nominal size.  A higher-priority
 * XDG root always wins over a lower-priority root.
 *
 * On success, *path_out receives a malloc-allocated path which the caller must
 * free.  On failure, *path_out is NULL and errno describes the error.  Missing
 * or unsupported icons are reported as ENOENT.  This function does not recurse
 * through icon trees and never executes icon values.
 */
int app_icon_resolve(const char *icon_name, unsigned int target_size,
                     char **path_out);

#endif

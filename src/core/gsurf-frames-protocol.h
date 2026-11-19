/*
 * gsurf-frames-protocol.h - The messages between gsurf and the frames
 * web-process extension
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Private to libgsurf (gsurf-frames.c) and modules/frames/webext/: both
 * sides of one WebKit user-message exchange, so they cannot drift apart.
 * Constants only (nothing for the introspection scanner).
 */

#ifndef GSURF_FRAMES_PROTOCOL_H
#define GSURF_FRAMES_PROTOCOL_H

/*
 * "gsurf-frames.list": no parameters.  The reply is "(s)": a JSON array
 * of the page's frames in document order, the main frame first:
 * {id, uri, main, parent (id or null), x, y, width, height, visible}.
 * x, y, width, height is the frame's content box in the view's CSS
 * pixels (the main frame's: its viewport).
 */
#define GSURF_FRAMES_MSG_LIST "gsurf-frames.list"

/*
 * "gsurf-frames.eval": "(sss)" -- the frame (its id in decimal, or its
 * site: "host", "host:port" or "*.host", matched on the URI's host), the
 * script, and a CSS selector the frame must contain to be chosen ("" for
 * none).  Several frames left: the one visible one, else NO_FRAME.  The script runs in the frame's page world.  The reply is
 * "(utsdd)": a GSURF_FRAMES_STATUS_*, the frame's id, the value (strings
 * as-is, anything else as JSON) or the error text, and the frame's
 * content-box origin in the view, read after the script ran.
 */
#define GSURF_FRAMES_MSG_EVAL "gsurf-frames.eval"

#define GSURF_FRAMES_EVAL_PARAMS "(sss)"
#define GSURF_FRAMES_EVAL_REPLY  "(utsdd)"

/* The status of an eval reply. */
#define GSURF_FRAMES_STATUS_OK       (0)   /* the value */
#define GSURF_FRAMES_STATUS_NO_FRAME (1)   /* nothing matches the frame */
#define GSURF_FRAMES_STATUS_SCRIPT   (2)   /* the script threw */

#endif /* GSURF_FRAMES_PROTOCOL_H */

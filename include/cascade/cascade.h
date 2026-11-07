/* cascade — an HTML and CSS user interface renderer in plain C.
 *
 *   CscdWindowOptions opts = cscdWindowOptions();
 *   opts.page = "ui/index.html";
 *   CscdWindow* win = cscdWindowNew(&opts);
 *   cscdElementOnClick(cscdViewGet(cscdWindowView(win), CSCD_SV("#save")), onSave, app);
 *   return cscdWindowRun(win);
 *
 * Everything is implemented here: HTML parsing, the CSS cascade, layout (block, inline, flex, grid,
 * tables), TrueType fonts, PNG / JPEG decoding, anti-aliased painting, and presentation through
 * Vulkan, OpenGL or GDI. */
#ifndef CASCADE_H
#define CASCADE_H

#include "cascade/bind.h"
#include "cascade/canvas.h"
#include "cascade/core.h"
#include "cascade/css.h"
#include "cascade/dom.h"
#include "cascade/font.h"
#include "cascade/image.h"
#include "cascade/layout.h"
#include "cascade/style.h"
#include "cascade/view.h"

#endif

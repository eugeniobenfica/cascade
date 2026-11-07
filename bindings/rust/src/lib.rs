//! cascade for Rust: the handle API of `cascadeC.h`, with closures for listeners, painters and
//! list rows, and strings converted both ways.
//!
//! ```no_run
//! let view = cascade::View::new();
//! view.load_html("<p id=p>Hello</p><button id=b>Go</button>").unwrap();
//! let p = view.query("#p").unwrap();
//! view.query("#b").unwrap().on("click", move |_ev| { let _ = p.set_text("Clicked"); }).unwrap();
//! ```
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::rc::Rc;

pub mod sys {
    //! The C functions, as `cascadeC.h` declares them.
    use std::ffi::{c_char, c_float, c_int, c_uint, c_void};
    pub type View = c_void;
    pub type Window = c_void;
    pub type Canvas = c_void;
    pub type Element = u64;
    pub type FreeFn = Option<unsafe extern "C" fn(*mut c_void)>;
    pub type Fn = Option<unsafe extern "C" fn(*mut c_void)>;
    #[repr(C)]
    pub struct Event {
        pub type_: *const c_char,
        pub target: Element,
        pub current_target: Element,
        pub x: c_float,
        pub y: c_float,
        pub offset_x: c_float,
        pub offset_y: c_float,
        pub delta_x: c_float,
        pub delta_y: c_float,
        pub button: c_int,
        pub buttons: c_int,
        pub detail: c_int,
        pub key: *const c_char,
        pub mods: c_uint,
        pub internal_: *mut c_void,
    }
    #[repr(C)]
    #[derive(Default, Debug, Clone, Copy)]
    pub struct FrameStats {
        pub style_ms: f64,
        pub layout_ms: f64,
        pub paint_ms: f64,
        pub styles_computed: c_int,
        pub styles_reused: c_int,
        pub painted_area: f64,
        pub partial_paint: c_int,
    }
    pub type EventFn = Option<unsafe extern "C" fn(*mut View, *const Event, *mut c_void)>;
    pub type PaintFn = Option<unsafe extern "C" fn(*mut Canvas, c_float, c_float, *mut c_void)>;
    pub type ListFn = Option<unsafe extern "C" fn(*mut View, Element, usize, *mut c_void)>;
    pub type EditMenuFn = Option<unsafe extern "C" fn(*mut View, c_int, *const c_float, c_uint, *mut c_void)>;
    pub type MagnifierFn = Option<unsafe extern "C" fn(*mut View, c_int, c_float, c_float, *mut c_void)>;
    pub type ClipboardSetFn = Option<unsafe extern "C" fn(*const c_char, *mut c_void)>;
    pub type ClipboardGetFn = Option<unsafe extern "C" fn(*mut c_void) -> *const c_char>;
    pub const ERR_REFUSED: c_int = 6;

    extern "C" {
        pub fn cscd_version() -> u32;
        pub fn cscd_last_error() -> *const c_char;
        pub fn cscd_free(p: *mut c_void);
        pub fn cscd_view_new() -> *mut View;
        pub fn cscd_view_delete(v: *mut View);
        pub fn cscd_view_load_file(v: *mut View, path: *const c_char) -> c_int;
        pub fn cscd_view_load_html(v: *mut View, html: *const c_char, base_dir: *const c_char) -> c_int;
        pub fn cscd_view_add_style_sheet(v: *mut View, css: *const c_char) -> c_int;
        pub fn cscd_view_set_viewport(v: *mut View, w: c_float, h: c_float, dpr: c_float) -> c_int;
        pub fn cscd_view_set_dark_mode(v: *mut View, dark: c_int) -> c_int;
        pub fn cscd_view_title(v: *mut View) -> *mut c_char;
        pub fn cscd_view_update(v: *mut View) -> c_int;
        pub fn cscd_view_paint(v: *mut View, w: *mut c_int, h: *mut c_int) -> *const u32;
        pub fn cscd_view_frame_stats(v: *mut View, out: *mut FrameStats) -> c_int;
        pub fn cscd_view_set_feature(v: *mut View, feature: c_int, on: c_int) -> c_int;
        pub fn cscd_view_feature(v: *mut View, feature: c_int) -> c_int;
        pub fn cscd_view_inspect(v: *mut View, e: Element) -> c_int;
        pub fn cscd_element_append_copy(v: *mut View, parent: Element, source: Element) -> Element;
        pub fn cscd_element_fill(v: *mut View, e: Element, key: *const c_char, value: *const c_char) -> c_int;
        pub fn cscd_view_bind_list(v: *mut View, box_: Element, row: Element, count: usize, row_height: c_float, fill: ListFn,
                                   user: *mut c_void, free_user: FreeFn) -> c_int;
        pub fn cscd_view_list_set_count(v: *mut View, box_: Element, count: usize) -> c_int;
        pub fn cscd_view_mouse_move(v: *mut View, x: c_float, y: c_float, buttons: c_int, mods: c_uint) -> c_int;
        pub fn cscd_view_mouse_down(v: *mut View, x: c_float, y: c_float, button: c_int, mods: c_uint, clicks: c_int) -> c_int;
        pub fn cscd_view_mouse_up(v: *mut View, x: c_float, y: c_float, button: c_int, mods: c_uint) -> c_int;
        pub fn cscd_view_text_input(v: *mut View, utf8: *const c_char) -> c_int;
        pub fn cscd_view_touch_down(v: *mut View, id: c_int, x: c_float, y: c_float) -> c_int;
        pub fn cscd_view_touch_move(v: *mut View, id: c_int, x: c_float, y: c_float) -> c_int;
        pub fn cscd_view_touch_up(v: *mut View, id: c_int, x: c_float, y: c_float) -> c_int;
        pub fn cscd_view_touch_cancel(v: *mut View) -> c_int;
        pub fn cscd_view_on_edit_menu(v: *mut View, f: EditMenuFn, user: *mut c_void, free_user: FreeFn) -> c_int;
        pub fn cscd_view_on_magnifier(v: *mut View, f: MagnifierFn, user: *mut c_void, free_user: FreeFn) -> c_int;
        pub fn cscd_view_edit_action(v: *mut View, action: c_int) -> c_int;
        pub fn cscd_view_edit_actions(v: *mut View) -> c_uint;
        pub fn cscd_view_selection_handles_shown(v: *mut View) -> c_int;
        pub fn cscd_view_hide_selection_handles(v: *mut View) -> c_int;
        pub fn cscd_view_set_clipboard(v: *mut View, set: ClipboardSetFn, get: ClipboardGetFn, user: *mut c_void, free_user: FreeFn) -> c_int;
        pub fn cscd_view_copy_text(v: *mut View, utf8: *const c_char) -> c_int;
        pub fn cscd_view_copy_image(v: *mut View, pixels: *const u32, width: c_int, height: c_int) -> c_int;
        pub fn cscd_view_clipboard_text(v: *mut View) -> *mut c_char;
        pub fn cscd_view_clipboard_files(v: *mut View) -> *mut c_char;
        pub fn cscd_view_set_timeout(v: *mut View, ms: c_int, f: Fn, user: *mut c_void, free_user: FreeFn) -> c_int;
        pub fn cscd_view_query(v: *mut View, selector: *const c_char) -> Element;
        pub fn cscd_view_query_all(v: *mut View, selector: *const c_char, out: *mut Element, capacity: usize) -> usize;
        pub fn cscd_view_body(v: *mut View) -> Element;
        pub fn cscd_element_valid(v: *mut View, e: Element) -> c_int;
        pub fn cscd_element_query(v: *mut View, e: Element, selector: *const c_char) -> Element;
        pub fn cscd_element_parent(v: *mut View, e: Element) -> Element;
        pub fn cscd_element_tag(v: *mut View, e: Element) -> *mut c_char;
        pub fn cscd_element_attribute(v: *mut View, e: Element, name: *const c_char) -> *mut c_char;
        pub fn cscd_element_set_attribute(v: *mut View, e: Element, name: *const c_char, value: *const c_char) -> c_int;
        pub fn cscd_element_add_class(v: *mut View, e: Element, name: *const c_char) -> c_int;
        pub fn cscd_element_remove_class(v: *mut View, e: Element, name: *const c_char) -> c_int;
        pub fn cscd_element_set_style(v: *mut View, e: Element, prop: *const c_char, value: *const c_char) -> c_int;
        pub fn cscd_element_text(v: *mut View, e: Element) -> *mut c_char;
        pub fn cscd_element_set_text(v: *mut View, e: Element, text: *const c_char) -> c_int;
        pub fn cscd_element_set_html(v: *mut View, e: Element, html: *const c_char) -> c_int;
        pub fn cscd_element_value(v: *mut View, e: Element) -> *mut c_char;
        pub fn cscd_element_set_value(v: *mut View, e: Element, value: *const c_char) -> c_int;
        pub fn cscd_element_append(v: *mut View, parent: Element, tag: *const c_char) -> Element;
        pub fn cscd_element_remove(v: *mut View, e: Element) -> c_int;
        pub fn cscd_element_click(v: *mut View, e: Element) -> c_int;
        pub fn cscd_element_focus(v: *mut View, e: Element) -> c_int;
        pub fn cscd_element_rect(v: *mut View, e: Element, out: *mut c_float) -> c_int;
        pub fn cscd_element_on(v: *mut View, e: Element, type_: *const c_char, f: EventFn, user: *mut c_void, free_user: FreeFn) -> c_int;
        pub fn cscd_event_prevent_default(ev: *const Event);
        pub fn cscd_element_set_painter(v: *mut View, e: Element, f: PaintFn, user: *mut c_void, free_user: FreeFn) -> c_int;
        pub fn cscd_canvas_fill_rect(c: *mut Canvas, x: c_float, y: c_float, w: c_float, h: c_float, rgba: u32);
        pub fn cscd_canvas_fill_circle(c: *mut Canvas, x: c_float, y: c_float, r: c_float, rgba: u32);
        pub fn cscd_canvas_draw_line(c: *mut Canvas, x0: c_float, y0: c_float, x1: c_float, y1: c_float, rgba: u32, width: c_float);
        pub fn cscd_canvas_fill_path(c: *mut Canvas, svg_path: *const c_char, rgba: u32);
        pub fn cscd_canvas_draw_text(c: *mut Canvas, utf8: *const c_char, x: c_float, y: c_float, family: *const c_char, size: c_float,
                                     weight: c_int, rgba: u32);
        pub fn cscd_window_new(title: *const c_char, w: c_int, h: c_int) -> *mut Window;
        pub fn cscd_window_delete(w: *mut Window);
        pub fn cscd_window_view(w: *mut Window) -> *mut View;
        pub fn cscd_window_run(w: *mut Window) -> c_int;
    }
}

/// What a call reports when it fails (cscd_last_error's message).
#[derive(Debug, Clone)]
pub struct Error(pub String);
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { f.write_str(&self.0) }
}
impl std::error::Error for Error {}
pub type Result<T> = std::result::Result<T, Error>;

fn last_error() -> Error {
    unsafe { Error(CStr::from_ptr(sys::cscd_last_error()).to_string_lossy().into_owned()) }
}
fn check(r: c_int) -> Result<()> { if r == 0 { Ok(()) } else { Err(last_error()) } }
fn cs(s: &str) -> CString { CString::new(s.replace('\0', "")).unwrap() }
fn take(p: *mut c_char) -> Option<String> {
    if p.is_null() {
        return None;
    }
    unsafe {
        let s = CStr::from_ptr(p).to_string_lossy().into_owned();
        sys::cscd_free(p as *mut c_void);
        Some(s)
    }
}

/// The features a view can turn off or on (CscdFeature).
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Feature {
    PaintThreads = 0, PartialRepaint, Animations, SmoothScroll, Tooltips, LiveReload, Accessibility, Inspector, Stats, Gpu,
    WarnFiles, WarnCss, WarnSelectors,
}

/// What an edit menu offers (CscdEditAction).
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum EditAction {
    Cut = 0, Copy, Paste, SelectAll,
}
impl EditAction {
    pub const ALL: [EditAction; 4] = [EditAction::Cut, EditAction::Copy, EditAction::Paste, EditAction::SelectAll];
    fn from_bits(bits: u32) -> Vec<EditAction> { Self::ALL.iter().copied().filter(|a| bits & (1 << *a as u32) != 0).collect() }
}

struct ViewInner {
    ptr: *mut sys::View,
    owned: bool,
}
impl Drop for ViewInner {
    fn drop(&mut self) {
        if self.owned {
            unsafe { sys::cscd_view_delete(self.ptr) }
        }
    }
}

/// A page. Cloning it gives another reference to the same view.
#[derive(Clone)]
pub struct View(Rc<ViewInner>);

/// An element: a handle that stays safe after the element is gone (calls then return an error).
#[derive(Clone)]
pub struct Element {
    view: View,
    handle: sys::Element,
}

/// An event, while its listener runs.
pub struct Event<'a> {
    raw: &'a sys::Event,
    view: View,
}
impl<'a> Event<'a> {
    pub fn kind(&self) -> String { unsafe { CStr::from_ptr(self.raw.type_).to_string_lossy().into_owned() } }
    pub fn target(&self) -> Option<Element> { self.view.wrap(self.raw.target) }
    pub fn position(&self) -> (f32, f32) { (self.raw.x, self.raw.y) }
    pub fn key(&self) -> String {
        if self.raw.key.is_null() { String::new() } else { unsafe { CStr::from_ptr(self.raw.key).to_string_lossy().into_owned() } }
    }
    pub fn prevent_default(&self) { unsafe { sys::cscd_event_prevent_default(self.raw) } }
}

/// The canvas a painter draws on. Colors are 0xRRGGBBAA.
pub struct Canvas(*mut sys::Canvas);
impl Canvas {
    pub fn fill_rect(&self, x: f32, y: f32, w: f32, h: f32, rgba: u32) { unsafe { sys::cscd_canvas_fill_rect(self.0, x, y, w, h, rgba) } }
    pub fn fill_circle(&self, x: f32, y: f32, r: f32, rgba: u32) { unsafe { sys::cscd_canvas_fill_circle(self.0, x, y, r, rgba) } }
    pub fn draw_line(&self, x0: f32, y0: f32, x1: f32, y1: f32, rgba: u32, width: f32) {
        unsafe { sys::cscd_canvas_draw_line(self.0, x0, y0, x1, y1, rgba, width) }
    }
    pub fn fill_path(&self, svg_path: &str, rgba: u32) { unsafe { sys::cscd_canvas_fill_path(self.0, cs(svg_path).as_ptr(), rgba) } }
    pub fn draw_text(&self, text: &str, x: f32, y: f32, family: &str, size: f32, weight: i32, rgba: u32) {
        unsafe { sys::cscd_canvas_draw_text(self.0, cs(text).as_ptr(), x, y, cs(family).as_ptr(), size, weight, rgba) }
    }
}

// Closures given to the library: boxed, freed by the library's free_user.
unsafe extern "C" fn drop_box<T>(p: *mut c_void) { drop(Box::from_raw(p as *mut T)) }

type EventBox = (View, Box<dyn FnMut(&Event)>);
unsafe extern "C" fn on_event(_v: *mut sys::View, ev: *const sys::Event, user: *mut c_void) {
    let b = &mut *(user as *mut EventBox);
    let event = Event { raw: &*ev, view: b.0.clone() };
    (b.1)(&event);
}
type PaintBox = Box<dyn FnMut(&Canvas, f32, f32)>;
unsafe extern "C" fn on_paint(c: *mut sys::Canvas, w: f32, h: f32, user: *mut c_void) {
    let f = &mut *(user as *mut PaintBox);
    f(&Canvas(c), w, h);
}
type ListBox = (View, Box<dyn FnMut(&Element, usize)>);
unsafe extern "C" fn on_list(_v: *mut sys::View, row: sys::Element, index: usize, user: *mut c_void) {
    let b = &mut *(user as *mut ListBox);
    if let Some(e) = b.0.wrap(row) {
        (b.1)(&e, index);
    }
}
type TimerBox = Box<dyn FnMut()>;
unsafe extern "C" fn on_timer(user: *mut c_void) { (*(user as *mut TimerBox))() }
type EditMenuBox = Box<dyn FnMut(bool, [f32; 4], Vec<EditAction>)>;
unsafe extern "C" fn on_edit_menu(_v: *mut sys::View, show: c_int, rect: *const f32, actions: u32, user: *mut c_void) {
    let r = std::slice::from_raw_parts(rect, 4);
    (*(user as *mut EditMenuBox))(show != 0, [r[0], r[1], r[2], r[3]], EditAction::from_bits(actions))
}
type MagnifierBox = Box<dyn FnMut(bool, f32, f32)>;
unsafe extern "C" fn on_magnifier(_v: *mut sys::View, show: c_int, x: f32, y: f32, user: *mut c_void) {
    (*(user as *mut MagnifierBox))(show != 0, x, y)
}
/// A clipboard of the program's: what text fields copy, and what they paste (kept here between calls).
struct ClipboardBox {
    set: Box<dyn FnMut(&str)>,
    get: Box<dyn FnMut() -> Option<String>>,
    last: Option<CString>,
}
unsafe extern "C" fn on_clipboard_set(text: *const c_char, user: *mut c_void) {
    let b = &mut *(user as *mut ClipboardBox);
    (b.set)(&CStr::from_ptr(text).to_string_lossy())
}
unsafe extern "C" fn on_clipboard_get(user: *mut c_void) -> *const c_char {
    let b = &mut *(user as *mut ClipboardBox);
    b.last = (b.get)().map(|s| cs(&s));
    b.last.as_ref().map_or(std::ptr::null(), |s| s.as_ptr())
}

impl View {
    pub fn new() -> View { View(Rc::new(ViewInner { ptr: unsafe { sys::cscd_view_new() }, owned: true })) }
    fn p(&self) -> *mut sys::View { self.0.ptr }
    fn wrap(&self, h: sys::Element) -> Option<Element> { if h == 0 { None } else { Some(Element { view: self.clone(), handle: h }) } }

    pub fn load_file(&self, path: &str) -> Result<()> { check(unsafe { sys::cscd_view_load_file(self.p(), cs(path).as_ptr()) }) }
    pub fn load_html(&self, html: &str) -> Result<()> {
        check(unsafe { sys::cscd_view_load_html(self.p(), cs(html).as_ptr(), std::ptr::null()) })
    }
    pub fn add_style_sheet(&self, css: &str) -> Result<()> { check(unsafe { sys::cscd_view_add_style_sheet(self.p(), cs(css).as_ptr()) }) }
    pub fn set_viewport(&self, w: f32, h: f32, dpr: f32) -> Result<()> { check(unsafe { sys::cscd_view_set_viewport(self.p(), w, h, dpr) }) }
    pub fn set_dark_mode(&self, dark: bool) -> Result<()> { check(unsafe { sys::cscd_view_set_dark_mode(self.p(), dark as c_int) }) }
    pub fn title(&self) -> Option<String> { take(unsafe { sys::cscd_view_title(self.p()) }) }
    pub fn update(&self) -> bool { unsafe { sys::cscd_view_update(self.p()) != 0 } }
    /// The frame (width, height, premultiplied BGRA pixels), good until the next paint.
    pub fn paint(&self) -> (usize, usize, &[u32]) {
        let (mut w, mut h) = (0 as c_int, 0 as c_int);
        let p = unsafe { sys::cscd_view_paint(self.p(), &mut w, &mut h) };
        let (w, h) = (w.max(0) as usize, h.max(0) as usize);
        if p.is_null() { (w, h, &[]) } else { (w, h, unsafe { std::slice::from_raw_parts(p, w * h) }) }
    }
    pub fn stats(&self) -> sys::FrameStats {
        let mut s = sys::FrameStats::default();
        unsafe { sys::cscd_view_frame_stats(self.p(), &mut s) };
        s
    }
    pub fn set_feature(&self, f: Feature, on: bool) -> Result<()> { check(unsafe { sys::cscd_view_set_feature(self.p(), f as c_int, on as c_int) }) }
    pub fn feature(&self, f: Feature) -> bool { unsafe { sys::cscd_view_feature(self.p(), f as c_int) != 0 } }
    pub fn inspect(&self, e: Option<&Element>) -> Result<()> {
        check(unsafe { sys::cscd_view_inspect(self.p(), e.map_or(0, |e| e.handle)) })
    }
    pub fn query(&self, selector: &str) -> Option<Element> { self.wrap(unsafe { sys::cscd_view_query(self.p(), cs(selector).as_ptr()) }) }
    pub fn query_all(&self, selector: &str) -> Vec<Element> {
        let sel = cs(selector);
        let n = unsafe { sys::cscd_view_query_all(self.p(), sel.as_ptr(), std::ptr::null_mut(), 0) };
        let mut out = vec![0 as sys::Element; n];
        let n = unsafe { sys::cscd_view_query_all(self.p(), sel.as_ptr(), out.as_mut_ptr(), n) };
        out.truncate(n);
        out.into_iter().filter_map(|h| self.wrap(h)).collect()
    }
    pub fn body(&self) -> Option<Element> { self.wrap(unsafe { sys::cscd_view_body(self.p()) }) }
    pub fn mouse_move(&self, x: f32, y: f32) -> Result<()> { check(unsafe { sys::cscd_view_mouse_move(self.p(), x, y, 0, 0) }) }
    pub fn click_at(&self, x: f32, y: f32) -> Result<()> {
        check(unsafe { sys::cscd_view_mouse_down(self.p(), x, y, 0, 0, 1) })?;
        check(unsafe { sys::cscd_view_mouse_up(self.p(), x, y, 0, 0) })
    }
    pub fn text_input(&self, text: &str) -> Result<()> { check(unsafe { sys::cscd_view_text_input(self.p(), cs(text).as_ptr()) }) }
    pub fn set_timeout(&self, ms: i32, f: impl FnMut() + 'static) -> i32 {
        let user = Box::into_raw(Box::new(Box::new(f) as TimerBox)) as *mut c_void;
        unsafe { sys::cscd_view_set_timeout(self.p(), ms, Some(on_timer), user, Some(drop_box::<TimerBox>)) }
    }
    /// A list of `count` rows of which only those in sight are made; `fill(row, index)` fills one.
    pub fn bind_list(&self, list: &Element, row: &Element, count: usize, row_height: f32, fill: impl FnMut(&Element, usize) + 'static) -> Result<()> {
        let user = Box::into_raw(Box::new((self.clone(), Box::new(fill) as Box<dyn FnMut(&Element, usize)>))) as *mut c_void;
        check(unsafe {
            sys::cscd_view_bind_list(self.p(), list.handle, row.handle, count, row_height, Some(on_list), user, Some(drop_box::<ListBox>))
        })
    }
    pub fn list_set_count(&self, list: &Element, count: usize) -> Result<()> {
        check(unsafe { sys::cscd_view_list_set_count(self.p(), list.handle, count) })
    }

    // touch, and selecting text by touch
    pub fn touch_down(&self, id: i32, x: f32, y: f32) -> Result<()> { check(unsafe { sys::cscd_view_touch_down(self.p(), id, x, y) }) }
    pub fn touch_move(&self, id: i32, x: f32, y: f32) -> Result<()> { check(unsafe { sys::cscd_view_touch_move(self.p(), id, x, y) }) }
    pub fn touch_up(&self, id: i32, x: f32, y: f32) -> Result<()> { check(unsafe { sys::cscd_view_touch_up(self.p(), id, x, y) }) }
    pub fn touch_cancel(&self) -> Result<()> { check(unsafe { sys::cscd_view_touch_cancel(self.p()) }) }
    /// `f(show, [x, y, width, height], actions)`: the edit menu next to a selection made by touch. A
    /// window's view has the system's; this replaces it.
    pub fn on_edit_menu(&self, f: impl FnMut(bool, [f32; 4], Vec<EditAction>) + 'static) -> Result<()> {
        let user = Box::into_raw(Box::new(Box::new(f) as EditMenuBox)) as *mut c_void;
        check(unsafe { sys::cscd_view_on_edit_menu(self.p(), Some(on_edit_menu), user, Some(drop_box::<EditMenuBox>)) })
    }
    /// `f(show, x, y)` while a selection handle is dragged.
    pub fn on_magnifier(&self, f: impl FnMut(bool, f32, f32) + 'static) -> Result<()> {
        let user = Box::into_raw(Box::new(Box::new(f) as MagnifierBox)) as *mut c_void;
        check(unsafe { sys::cscd_view_on_magnifier(self.p(), Some(on_magnifier), user, Some(drop_box::<MagnifierBox>)) })
    }
    /// Cut, copy, paste or select all in the focused text field: `Ok(false)` when it cannot now.
    pub fn edit_action(&self, action: EditAction) -> Result<bool> {
        match unsafe { sys::cscd_view_edit_action(self.p(), action as c_int) } {
            0 => Ok(true),
            sys::ERR_REFUSED => Ok(false),
            _ => Err(last_error()),
        }
    }
    pub fn edit_actions(&self) -> Vec<EditAction> { EditAction::from_bits(unsafe { sys::cscd_view_edit_actions(self.p()) }) }
    pub fn selection_handles_shown(&self) -> bool { unsafe { sys::cscd_view_selection_handles_shown(self.p()) != 0 } }
    pub fn hide_selection_handles(&self) -> Result<()> { check(unsafe { sys::cscd_view_hide_selection_handles(self.p()) }) }

    // the clipboard
    /// A clipboard of your own, for a view without a window.
    pub fn set_clipboard(&self, set: impl FnMut(&str) + 'static, get: impl FnMut() -> Option<String> + 'static) -> Result<()> {
        let user = Box::into_raw(Box::new(ClipboardBox { set: Box::new(set), get: Box::new(get), last: None })) as *mut c_void;
        check(unsafe {
            sys::cscd_view_set_clipboard(self.p(), Some(on_clipboard_set), Some(on_clipboard_get), user, Some(drop_box::<ClipboardBox>))
        })
    }
    /// `false` without a clipboard.
    pub fn copy_text(&self, text: &str) -> bool { unsafe { sys::cscd_view_copy_text(self.p(), cs(text).as_ptr()) == 0 } }
    /// Premultiplied BGRA pixels, rows packed: `false` without a clipboard for pictures.
    pub fn copy_image(&self, width: usize, height: usize, pixels: &[u32]) -> bool {
        if pixels.len() < width * height {
            return false;
        }
        unsafe { sys::cscd_view_copy_image(self.p(), pixels.as_ptr(), width as c_int, height as c_int) == 0 }
    }
    pub fn clipboard_text(&self) -> String { take(unsafe { sys::cscd_view_clipboard_text(self.p()) }).unwrap_or_default() }
    /// The clipboard's files (on phones, copies; a picture as a PNG file).
    pub fn clipboard_files(&self) -> Vec<String> {
        take(unsafe { sys::cscd_view_clipboard_files(self.p()) }).unwrap_or_default().lines().filter(|l| !l.is_empty()).map(String::from).collect()
    }
}

impl Default for View {
    fn default() -> Self { View::new() }
}

impl Element {
    fn p(&self) -> *mut sys::View { self.view.p() }
    pub fn valid(&self) -> bool { unsafe { sys::cscd_element_valid(self.p(), self.handle) != 0 } }
    pub fn tag(&self) -> Option<String> { take(unsafe { sys::cscd_element_tag(self.p(), self.handle) }) }
    pub fn parent(&self) -> Option<Element> { self.view.wrap(unsafe { sys::cscd_element_parent(self.p(), self.handle) }) }
    pub fn query(&self, selector: &str) -> Option<Element> {
        self.view.wrap(unsafe { sys::cscd_element_query(self.p(), self.handle, cs(selector).as_ptr()) })
    }
    pub fn attribute(&self, name: &str) -> Option<String> { take(unsafe { sys::cscd_element_attribute(self.p(), self.handle, cs(name).as_ptr()) }) }
    pub fn set_attribute(&self, name: &str, value: &str) -> Result<()> {
        check(unsafe { sys::cscd_element_set_attribute(self.p(), self.handle, cs(name).as_ptr(), cs(value).as_ptr()) })
    }
    pub fn add_class(&self, name: &str) -> Result<()> { check(unsafe { sys::cscd_element_add_class(self.p(), self.handle, cs(name).as_ptr()) }) }
    pub fn remove_class(&self, name: &str) -> Result<()> { check(unsafe { sys::cscd_element_remove_class(self.p(), self.handle, cs(name).as_ptr()) }) }
    pub fn set_style(&self, prop: &str, value: &str) -> Result<()> {
        check(unsafe { sys::cscd_element_set_style(self.p(), self.handle, cs(prop).as_ptr(), cs(value).as_ptr()) })
    }
    pub fn text(&self) -> Option<String> { take(unsafe { sys::cscd_element_text(self.p(), self.handle) }) }
    pub fn set_text(&self, text: &str) -> Result<()> { check(unsafe { sys::cscd_element_set_text(self.p(), self.handle, cs(text).as_ptr()) }) }
    pub fn set_html(&self, html: &str) -> Result<()> { check(unsafe { sys::cscd_element_set_html(self.p(), self.handle, cs(html).as_ptr()) }) }
    pub fn value(&self) -> Option<String> { take(unsafe { sys::cscd_element_value(self.p(), self.handle) }) }
    pub fn set_value(&self, value: &str) -> Result<()> { check(unsafe { sys::cscd_element_set_value(self.p(), self.handle, cs(value).as_ptr()) }) }
    pub fn append(&self, tag: &str) -> Option<Element> { self.view.wrap(unsafe { sys::cscd_element_append(self.p(), self.handle, cs(tag).as_ptr()) }) }
    /// A copy of `source` (a `<template>`'s first element, or any element) appended here.
    pub fn append_copy(&self, source: &Element) -> Result<Element> {
        self.view.wrap(unsafe { sys::cscd_element_append_copy(self.p(), self.handle, source.handle) }).ok_or_else(last_error)
    }
    /// Replaces `{{key}}` with `value` in the text and attributes of this element and what it holds.
    pub fn fill(&self, key: &str, value: &str) -> Result<()> {
        check(unsafe { sys::cscd_element_fill(self.p(), self.handle, cs(key).as_ptr(), cs(value).as_ptr()) })
    }
    pub fn remove(&self) -> Result<()> { check(unsafe { sys::cscd_element_remove(self.p(), self.handle) }) }
    pub fn click(&self) -> Result<()> { check(unsafe { sys::cscd_element_click(self.p(), self.handle) }) }
    pub fn focus(&self) -> Result<()> { check(unsafe { sys::cscd_element_focus(self.p(), self.handle) }) }
    /// The border box after layout, relative to the viewport: x, y, width, height.
    pub fn rect(&self) -> Result<[f32; 4]> {
        let mut r = [0f32; 4];
        check(unsafe { sys::cscd_element_rect(self.p(), self.handle, r.as_mut_ptr()) }).map(|_| r)
    }
    /// `f` for each event of that kind here or under (as addEventListener).
    pub fn on(&self, kind: &str, f: impl FnMut(&Event) + 'static) -> Result<()> {
        let user = Box::into_raw(Box::new((self.view.clone(), Box::new(f) as Box<dyn FnMut(&Event)>))) as *mut c_void;
        check(unsafe { sys::cscd_element_on(self.p(), self.handle, cs(kind).as_ptr(), Some(on_event), user, Some(drop_box::<EventBox>)) })
    }
    /// `f(canvas, width, height)` draws this element's content whenever it is painted.
    pub fn set_painter(&self, f: impl FnMut(&Canvas, f32, f32) + 'static) -> Result<()> {
        let user = Box::into_raw(Box::new(Box::new(f) as PaintBox)) as *mut c_void;
        check(unsafe { sys::cscd_element_set_painter(self.p(), self.handle, Some(on_paint), user, Some(drop_box::<PaintBox>)) })
    }
}

/// A native window showing a view.
pub struct Window {
    ptr: *mut sys::Window,
    view: View,
}
impl Window {
    pub fn new(title: &str, width: i32, height: i32) -> Result<Window> {
        let ptr = unsafe { sys::cscd_window_new(cs(title).as_ptr(), width, height) };
        if ptr.is_null() {
            return Err(last_error());
        }
        let view = View(Rc::new(ViewInner { ptr: unsafe { sys::cscd_window_view(ptr) }, owned: false }));
        Ok(Window { ptr, view })
    }
    pub fn view(&self) -> &View { &self.view }
    pub fn run(&self) -> i32 { unsafe { sys::cscd_window_run(self.ptr) } }
}
impl Drop for Window {
    fn drop(&mut self) { unsafe { sys::cscd_window_delete(self.ptr) } }
}

pub fn version() -> (u32, u32, u32) {
    let v = unsafe { sys::cscd_version() };
    (v >> 16, (v >> 8) & 255, v & 255)
}

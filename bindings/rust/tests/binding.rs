// The Rust binding over the library: cargo test (CASCADE_LIB_DIR names the folder with cascade_c,
// which must also be where the system finds it when the test runs).
use cascade::{EditAction, View};
use std::cell::RefCell;
use std::rc::Rc;

#[test]
fn events_and_values() {
    let view = View::new();
    view.load_html("<p id=p>olá</p><button id=b>Go</button><input id=name value=Ann>").unwrap();
    view.set_viewport(400.0, 300.0, 1.0).unwrap();
    view.update();
    let p = view.query("#p").unwrap();
    assert_eq!(p.text().as_deref(), Some("olá"));
    let q = p.clone();
    view.query("#b").unwrap().on("click", move |_| { q.set_text("Clicked").unwrap(); }).unwrap();
    view.query("#b").unwrap().click().unwrap();
    assert_eq!(p.text().as_deref(), Some("Clicked"));
    p.remove().unwrap();
    assert!(p.set_text("x").is_err());
}

#[test]
fn touch_selection_and_clipboard() {
    let view = View::new();
    view.load_html("<style>body{margin:0} input{margin:0;width:300px;font-size:20px}</style><input id=f value='alpha beta'>").unwrap();
    view.set_viewport(400.0, 200.0, 1.0).unwrap();
    view.update();
    view.paint(); // (taps find what was painted)
    let clip = Rc::new(RefCell::new(String::new()));
    let (c1, c2) = (clip.clone(), clip.clone());
    view.set_clipboard(move |t| *c1.borrow_mut() = t.to_string(), move || Some(c2.borrow().clone())).unwrap();
    let menus = Rc::new(RefCell::new(Vec::new()));
    let m = menus.clone();
    view.on_edit_menu(move |show, rect, actions| m.borrow_mut().push((show, rect, actions))).unwrap();
    let f = view.query("#f").unwrap();
    let [x, y, _, h] = f.rect().unwrap();
    assert_eq!(view.edit_action(EditAction::Copy).unwrap(), false);
    for _ in 0..2 {
        view.touch_down(1, x + 12.0, y + h / 2.0).unwrap();
        view.touch_up(1, x + 12.0, y + h / 2.0).unwrap();
    }
    assert!(view.selection_handles_shown());
    {
        let menus = menus.borrow();
        let (show, rect, actions) = menus.last().expect("an edit menu");
        assert!(*show && rect[2] > 0.0 && actions.contains(&EditAction::Copy));
    }
    assert!(view.edit_actions().contains(&EditAction::Cut));
    assert!(view.edit_action(EditAction::Copy).unwrap());
    assert_eq!(*clip.borrow(), "alpha");
    assert!(view.edit_action(EditAction::SelectAll).unwrap() && view.edit_action(EditAction::Cut).unwrap());
    assert_eq!(f.value().as_deref(), Some(""));
    assert!(view.edit_action(EditAction::Paste).unwrap());
    assert_eq!(f.value().as_deref(), Some("alpha beta"));
    view.hide_selection_handles().unwrap();
    assert!(!view.selection_handles_shown());
    assert!(view.copy_text("from rust") && view.clipboard_text() == "from rust");
    assert!(!view.copy_image(1, 1, &[0xFFFF0000]) && view.clipboard_files().is_empty());
    view.touch_cancel().unwrap();
}

package org.cascade;

import android.text.Editable;
import android.text.Selection;
import android.view.KeyEvent;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;

import static org.cascade.CascadeActivity.*;

/* The connection the keyboard edits through: the activity's editable, each change sent to the C side. */
/* The connection the keyboard edits through: the editable above, each change sent to the C side. */
class CascadeInputConnection extends BaseInputConnection {
    final CascadeActivity activity;
    int batch;
    CascadeInputConnection(CascadePageView v) { super(v, true); activity = v.activity; }
    @Override public Editable getEditable() { return activity.editable; }
    @Override public boolean beginBatchEdit() { ++batch; return true; }
    @Override public boolean endBatchEdit() {
        if (batch > 0 && --batch == 0) send();
        return batch > 0;
    }
    void changed() { if (batch == 0) send(); }
    void send() {
        String s = activity.editable.toString();
        int a = Selection.getSelectionStart(activity.editable), z = Selection.getSelectionEnd(activity.editable);
        int ca = getComposingSpanStart(activity.editable), cz = getComposingSpanEnd(activity.editable);
        int[] ints = {bytesBefore(s, a), bytesBefore(s, z), ca >= 0 ? bytesBefore(s, Math.min(ca, cz)) : 0, ca >= 0 ? bytesBefore(s, Math.max(ca, cz)) : 0};
        nativeEvent(E_TEXT_STATE, ints, null, utf8(s));
    }
    @Override public boolean commitText(CharSequence t, int pos) {
        /* a line break in a single line field is the Enter key (its action) */
        if (activity.inputKind != K_MULTILINE && "\n".contentEquals(t)) { performEditorAction(EditorInfo.IME_ACTION_UNSPECIFIED); return true; }
        boolean r = super.commitText(t, pos); changed(); return r;
    }
    @Override public boolean setComposingText(CharSequence t, int pos) { boolean r = super.setComposingText(t, pos); changed(); return r; }
    @Override public boolean setComposingRegion(int a, int z) { boolean r = super.setComposingRegion(a, z); changed(); return r; }
    @Override public boolean finishComposingText() { boolean r = super.finishComposingText(); changed(); return r; }
    @Override public boolean deleteSurroundingText(int before, int after) { boolean r = super.deleteSurroundingText(before, after); changed(); return r; }
    @Override public boolean deleteSurroundingTextInCodePoints(int before, int after) {
        boolean r = super.deleteSurroundingTextInCodePoints(before, after); changed(); return r;
    }
    @Override public boolean setSelection(int a, int z) { boolean r = super.setSelection(a, z); changed(); return r; }
    @Override public boolean performEditorAction(int action) {
        nativeEvent(E_EDITOR_ACTION, new int[] {activity.enterKey}, null, null);
        return true;
    }
    @Override public boolean sendKeyEvent(KeyEvent e) {
        /* keys the keyboard sends (Delete with nothing to delete, Enter, arrows) go as keys */
        activity.sendKey(e);
        return true;
    }
}

package org.cascade;

import android.os.Build;
import android.os.SystemClock;
import android.text.InputType;
import android.text.Selection;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

import static org.cascade.CascadeActivity.*;

/* The page's view: the surface the C side draws into, touch, the mouse and keys, the input method
 * and TalkBack's hover. */
final class CascadePageView extends SurfaceView implements SurfaceHolder.Callback {
    final CascadeActivity activity;
    CascadePageView(CascadeActivity c) {
        super(c);
        activity = c;
        getHolder().addCallback(this);
    }

    @Override public void surfaceCreated(SurfaceHolder h) { nativeSurface(S_CREATED, h.getSurface(), getWidth(), getHeight()); }
    @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int hh) { nativeSurface(S_CHANGED, h.getSurface(), w, hh); }
    @Override public void surfaceDestroyed(SurfaceHolder h) { nativeSurface(S_DESTROYED, null, 0, 0); }

    @Override public boolean onTouchEvent(MotionEvent e) {
        int tool = e.getToolType(e.getActionIndex());
        if (tool == MotionEvent.TOOL_TYPE_MOUSE) return mouse(e);
        /* a move brings the samples the system gathered since the last one (touch screens sample faster
         * than frames come): each goes, with how long ago it was, for a stroke's points and a fling's speed */
        int n = e.getPointerCount(), action = e.getActionMasked();
        int h = action == MotionEvent.ACTION_MOVE ? e.getHistorySize() : 0;
        int[] ints = new int[4 + n];
        float[] f = new float[(h + 1) * (1 + 2 * n)];
        ints[0] = action;
        ints[1] = e.getActionIndex();
        ints[2] = n;
        for (int i = 0; i < n; ++i) ints[3 + i] = e.getPointerId(i);
        ints[3 + n] = h + 1;
        long now = SystemClock.uptimeMillis();
        int k = 0;
        for (int s = 0; s <= h; ++s) {
            f[k++] = now - (s < h ? e.getHistoricalEventTime(s) : e.getEventTime());
            for (int i = 0; i < n; ++i) {
                f[k++] = s < h ? e.getHistoricalX(i, s) : e.getX(i);
                f[k++] = s < h ? e.getHistoricalY(i, s) : e.getY(i);
            }
        }
        nativeEvent(E_TOUCH, ints, f, null);
        return true;
    }

    @Override public boolean onGenericMotionEvent(MotionEvent e) {
        if (e.getActionMasked() == MotionEvent.ACTION_SCROLL || e.getActionMasked() == MotionEvent.ACTION_HOVER_MOVE
            || e.getActionMasked() == MotionEvent.ACTION_HOVER_ENTER || e.getActionMasked() == MotionEvent.ACTION_HOVER_EXIT)
            return mouse(e);
        return super.onGenericMotionEvent(e);
    }

    boolean mouse(MotionEvent e) {
        nativeEvent(E_MOUSE, new int[] {e.getActionMasked(), e.getButtonState(), e.getMetaState()},
                    new float[] {e.getX(), e.getY(), e.getAxisValue(MotionEvent.AXIS_HSCROLL), e.getAxisValue(MotionEvent.AXIS_VSCROLL)}, null);
        return true;
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int k = e.getKeyCode();
        /* the system's keys stay the system's; Back goes through onBackPressed */
        if (k == KeyEvent.KEYCODE_VOLUME_UP || k == KeyEvent.KEYCODE_VOLUME_DOWN || k == KeyEvent.KEYCODE_VOLUME_MUTE || k == KeyEvent.KEYCODE_POWER
            || k == KeyEvent.KEYCODE_HOME || k == KeyEvent.KEYCODE_APP_SWITCH || k == KeyEvent.KEYCODE_BACK)
            return super.dispatchKeyEvent(e);
        activity.sendKey(e);
        return true;
    }

    @Override public boolean onCheckIsTextEditor() { return activity.inputKind != K_NONE; }

    @Override public InputConnection onCreateInputConnection(EditorInfo info) {
        if (activity.inputKind == K_NONE) return null;
        int type = InputType.TYPE_CLASS_TEXT;
        switch (activity.inputKind) {
        case K_MULTILINE: type |= InputType.TYPE_TEXT_FLAG_MULTI_LINE; break;
        case K_NUMBER: type = InputType.TYPE_CLASS_NUMBER; break;
        case K_DECIMAL: type = InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL | InputType.TYPE_NUMBER_FLAG_SIGNED; break;
        case K_TEL: type = InputType.TYPE_CLASS_PHONE; break;
        case K_EMAIL: type |= InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS; break;
        case K_URL: type |= InputType.TYPE_TEXT_VARIATION_URI; break;
        case K_PASSWORD: type |= InputType.TYPE_TEXT_VARIATION_PASSWORD; break;
        default: break;
        }
        if ((type & InputType.TYPE_MASK_CLASS) == InputType.TYPE_CLASS_TEXT && activity.inputKind != K_PASSWORD) {
            if (activity.autocorrect) type |= InputType.TYPE_TEXT_FLAG_AUTO_CORRECT;
            else type |= InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
            if (activity.capitalize == 1) type |= InputType.TYPE_TEXT_FLAG_CAP_SENTENCES;
            else if (activity.capitalize == 2) type |= InputType.TYPE_TEXT_FLAG_CAP_WORDS;
            else if (activity.capitalize == 3) type |= InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS;
        }
        info.inputType = type;
        int action = EditorInfo.IME_ACTION_UNSPECIFIED;
        switch (activity.enterKey) {
        case ENTER_DONE: action = EditorInfo.IME_ACTION_DONE; break;
        case ENTER_GO: action = EditorInfo.IME_ACTION_GO; break;
        case ENTER_NEXT: action = EditorInfo.IME_ACTION_NEXT; break;
        case ENTER_PREVIOUS: action = EditorInfo.IME_ACTION_PREVIOUS; break;
        case ENTER_SEARCH: action = EditorInfo.IME_ACTION_SEARCH; break;
        case ENTER_SEND: action = EditorInfo.IME_ACTION_SEND; break;
        case ENTER_ENTER: action = EditorInfo.IME_ACTION_NONE; break;
        default: if (activity.inputKind != K_MULTILINE) action = EditorInfo.IME_ACTION_DONE; break;
        }
        info.imeOptions = action | EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
        if (activity.inputKind == K_MULTILINE && activity.enterKey == ENTER_DEFAULT) info.imeOptions |= EditorInfo.IME_FLAG_NO_ENTER_ACTION;
        info.initialSelStart = Selection.getSelectionStart(activity.editable);
        info.initialSelEnd = Selection.getSelectionEnd(activity.editable);
        if (Build.VERSION.SDK_INT >= 30) info.setInitialSurroundingText(activity.editable);
        activity.connection = new CascadeInputConnection(this);
        return activity.connection;
    }

    /* TalkBack: the page's accessible elements as the view's virtual children */
    @Override public AccessibilityNodeProvider getAccessibilityNodeProvider() { return activity.tree; }

    @Override public boolean dispatchHoverEvent(MotionEvent e) {
        if (!activity.a11y.isTouchExplorationEnabled() || activity.tree.nodes.isEmpty()) return super.dispatchHoverEvent(e);
        int id = activity.tree.at(e.getX(), e.getY());
        switch (e.getActionMasked()) {
        case MotionEvent.ACTION_HOVER_ENTER: case MotionEvent.ACTION_HOVER_MOVE:
            if (id != activity.tree.hovered) {
                activity.tree.send(id, AccessibilityEvent.TYPE_VIEW_HOVER_ENTER);
                activity.tree.send(activity.tree.hovered, AccessibilityEvent.TYPE_VIEW_HOVER_EXIT);
                activity.tree.hovered = id;
            }
            return true;
        case MotionEvent.ACTION_HOVER_EXIT:
            activity.tree.send(activity.tree.hovered, AccessibilityEvent.TYPE_VIEW_HOVER_EXIT);
            activity.tree.hovered = View.NO_ID;
            return true;
        default: return true;
        }
    }
}

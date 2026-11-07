package org.cascade;

import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

import static org.cascade.CascadeActivity.*;

/* TalkBack: the page's accessible tree, a copy of the one the C side sends, as the system's nodes; and
 * the events that tell TalkBack what changed. */
final class CascadeAccessibility extends AccessibilityNodeProvider {
    final CascadeActivity activity;
    CascadeAccessibility(CascadeActivity a) { activity = a; }

    static class Node {
        int id, parent, role, state, level;
        Rect bounds = new Rect();
        float value, min, max, step;
        boolean range;
        String name = "", description = "", text = "";
        List<Integer> children = new ArrayList<Integer>();
    }

    final HashMap<Integer, Node> nodes = new HashMap<Integer, Node>();
    final List<Integer> top = new ArrayList<Integer>();
    int focused = View.NO_ID, hovered = View.NO_ID;

    void load(int[] ints, float[] floats, byte[] strings) {
        nodes.clear();
        top.clear();
        List<Node> order = new ArrayList<Node>(); /* (the reading order) */
        int at = 0;
        if (ints == null || floats == null || strings == null) return;
        for (int i = 0; i * 10 + 9 < ints.length && i * 4 + 3 < floats.length; ++i) {
            Node n = new Node();
            n.id = ints[i * 10];
            n.parent = ints[i * 10 + 1];
            n.role = ints[i * 10 + 2];
            n.state = ints[i * 10 + 3];
            n.level = ints[i * 10 + 4];
            n.bounds.set(ints[i * 10 + 5], ints[i * 10 + 6], ints[i * 10 + 7], ints[i * 10 + 8]);
            n.range = ints[i * 10 + 9] != 0;
            n.value = floats[i * 4];
            n.min = floats[i * 4 + 1];
            n.max = floats[i * 4 + 2];
            n.step = floats[i * 4 + 3];
            String[] s = new String[3];
            for (int k = 0; k < 3; ++k) {
                int end = at;
                while (end < strings.length && strings[end] != 0) ++end;
                s[k] = new String(strings, at, end - at, StandardCharsets.UTF_8);
                at = end + 1;
            }
            n.name = s[0];
            n.description = s[1];
            n.text = s[2];
            nodes.put(n.id, n);
            order.add(n);
        }
        for (Node n : order) {
            Node p = nodes.get(n.parent);
            if (p != null) p.children.add(n.id);
            else top.add(n.id);
        }
        if (focused != View.NO_ID && !nodes.containsKey(focused)) focused = View.NO_ID;
    }

    /* the innermost element under a point of the view */
    int at(float x, float y) {
        int best = View.NO_ID;
        long area = Long.MAX_VALUE;
        for (Node n : nodes.values()) {
            if (!n.bounds.contains((int) x, (int) y)) continue;
            long a = (long) n.bounds.width() * n.bounds.height();
            if (a <= area) { area = a; best = n.id; }
        }
        return best;
    }

    void send(int id, int type) {
        if (!activity.a11y.isEnabled() || activity.page.getParent() == null) return;
        AccessibilityEvent e = AccessibilityEvent.obtain(type);
        e.setPackageName(activity.getPackageName());
        if (id == View.NO_ID) {
            e.setSource(activity.page);
            e.setClassName(activity.page.getClass().getName());
        } else {
            Node n = nodes.get(id);
            if (n == null) return;
            e.setSource(activity.page, id);
            e.setClassName(className(n));
            e.getText().add(label(n));
        }
        activity.page.getParent().requestSendAccessibilityEvent(activity.page, e);
    }

    String className(Node n) {
        switch (n.role) {
        case 4: case 5: return "android.widget.Button";          /* button, link */
        case 6: return "android.widget.CheckBox";
        case 7: return "android.widget.RadioButton";
        case 8: case 9: return "android.widget.EditText";        /* textbox, combobox */
        case 10: case 15: return "android.widget.ListView";      /* listbox, list */
        case 12: return "android.widget.SeekBar";
        case 13: return "android.widget.ProgressBar";
        case 17: return "android.widget.ImageView";
        case 20: return "android.widget.TabWidget";
        case 25: return "android.widget.GridView";               /* table */
        default: return "android.view.View";
        }
    }

    String label(Node n) {
        if (!n.name.isEmpty()) return n.name;
        return n.text;
    }

    @Override public AccessibilityNodeInfo createAccessibilityNodeInfo(int id) {
        if (id == View.NO_ID) {
            AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain(activity.page);
            activity.page.onInitializeAccessibilityNodeInfo(info);
            for (int c : top) info.addChild(activity.page, c);
            return info;
        }
        Node n = nodes.get(id);
        if (n == null) return null;
        AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain(activity.page, id);
        info.setPackageName(activity.getPackageName());
        info.setClassName(className(n));
        info.setParent(activity.page, nodes.containsKey(n.parent) ? n.parent : View.NO_ID);
        for (int c : n.children) info.addChild(activity.page, c);
        int[] loc = new int[2];
        activity.page.getLocationOnScreen(loc);
        Rect screen = new Rect(n.bounds);
        screen.offset(loc[0], loc[1]);
        info.setBoundsInScreen(screen);
        info.setVisibleToUser(Rect.intersects(n.bounds, new Rect(0, 0, activity.page.getWidth(), activity.page.getHeight())));
        info.setEnabled((n.state & ST_DISABLED) == 0);
        boolean editable = n.role == 8 && (n.state & ST_READONLY) == 0;
        if (n.role == 8 || n.role == 9) {
            info.setText(n.text);
            if (Build.VERSION.SDK_INT >= 26) info.setHintText(n.name);
            info.setEditable(editable);
            info.setPassword((n.state & ST_PASSWORD) != 0);
            info.setMultiLine((n.state & ST_MULTILINE) != 0);
            if (editable) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_TEXT);
        } else if (n.role == 3 || n.role == 14) {
            info.setText(n.name.isEmpty() ? n.text : n.name);
        } else {
            info.setContentDescription(label(n));
        }
        if (!n.description.isEmpty() && Build.VERSION.SDK_INT >= 30) info.setStateDescription(n.description);
        if (Build.VERSION.SDK_INT >= 28) info.setHeading(n.role == 14);
        info.setCheckable(n.role == 6 || n.role == 7);
        info.setChecked((n.state & ST_CHECKED) != 0);
        info.setSelected((n.state & ST_SELECTED) != 0);
        info.setFocusable((n.state & ST_FOCUSABLE) != 0);
        info.setFocused((n.state & ST_FOCUSED) != 0);
        info.setAccessibilityFocused(focused == id);
        boolean clickable = n.role == 4 || n.role == 5 || n.role == 6 || n.role == 7 || n.role == 9 || n.role == 11 || n.role == 19
                            || n.role == 24 || (n.state & ST_FOCUSABLE) != 0;
        info.setClickable(clickable && (n.state & ST_DISABLED) == 0);
        if (info.isClickable()) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_CLICK);
        if (info.isFocusable()) info.addAction(focused == id ? AccessibilityNodeInfo.AccessibilityAction.ACTION_CLEAR_FOCUS
                                                             : AccessibilityNodeInfo.AccessibilityAction.ACTION_FOCUS);
        info.addAction(focused == id ? AccessibilityNodeInfo.AccessibilityAction.ACTION_CLEAR_ACCESSIBILITY_FOCUS
                                     : AccessibilityNodeInfo.AccessibilityAction.ACTION_ACCESSIBILITY_FOCUS);
        info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SHOW_ON_SCREEN);
        if (n.range) {
            info.setRangeInfo(AccessibilityNodeInfo.RangeInfo.obtain(AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_FLOAT, n.min, n.max, n.value));
            if (n.role == 12) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_PROGRESS);
        }
        return info;
    }

    @Override public boolean performAction(int id, int action, Bundle args) {
        if (id == View.NO_ID) return activity.page.performAccessibilityAction(action, args);
        Node n = nodes.get(id);
        if (n == null) return false;
        switch (action) {
        case AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS:
            if (focused == id) return false;
            send(focused, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
            focused = id;
            activity.page.invalidate();
            send(id, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED);
            nativeEvent(E_A11Y_ACTION, new int[] {id, A_SCROLL_INTO_VIEW}, null, null);
            return true;
        case AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS:
            if (focused != id) return false;
            focused = View.NO_ID;
            send(id, AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);
            return true;
        case AccessibilityNodeInfo.ACTION_CLICK:
            nativeEvent(E_A11Y_ACTION, new int[] {id, A_ACTIVATE}, null, null);
            send(id, AccessibilityEvent.TYPE_VIEW_CLICKED);
            return true;
        case AccessibilityNodeInfo.ACTION_FOCUS:
            nativeEvent(E_A11Y_ACTION, new int[] {id, A_FOCUS}, null, null);
            return true;
        case AccessibilityNodeInfo.ACTION_SET_TEXT: {
            CharSequence t = args == null ? null : args.getCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE);
            nativeEvent(E_A11Y_ACTION, new int[] {id, A_SET_VALUE}, null, utf8(t == null ? "" : t.toString()));
            return true;
        }
        default:
            if (action == android.R.id.accessibilityActionShowOnScreen) {
                nativeEvent(E_A11Y_ACTION, new int[] {id, A_SCROLL_INTO_VIEW}, null, null);
                return true;
            }
            if (action == android.R.id.accessibilityActionSetProgress && args != null) {
                float v = args.getFloat(AccessibilityNodeInfo.ACTION_ARGUMENT_PROGRESS_VALUE);
                nativeEvent(E_A11Y_ACTION, new int[] {id, A_SET_VALUE}, null, utf8(Float.toString(v)));
                return true;
            }
            return false;
        }
    }

    @Override public AccessibilityNodeInfo findFocus(int type) {
        if (type == AccessibilityNodeInfo.FOCUS_ACCESSIBILITY) return focused == View.NO_ID ? null : createAccessibilityNodeInfo(focused);
        for (Node n : nodes.values())
            if ((n.state & ST_FOCUSED) != 0) return createAccessibilityNodeInfo(n.id);
        return null;
    }
}

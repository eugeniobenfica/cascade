package org.cascade;

import android.graphics.Rect;
import android.view.ActionMode;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;

import static org.cascade.CascadeActivity.*;

/* The floating edit menu over a selection made by touch (cut, copy, paste, select all), in the
 * system's own words. */
final class CascadeEditMenu extends ActionMode.Callback2 {
    final CascadeActivity activity;
    CascadeEditMenu(CascadeActivity a) { activity = a; }
    @Override public boolean onCreateActionMode(ActionMode mode, Menu menu) { return true; }
    @Override public boolean onPrepareActionMode(ActionMode mode, Menu menu) {
        menu.clear();
        /* the system's own labels, in the user's language */
        if ((activity.editActions & (1 << EDIT_CUT)) != 0) menu.add(Menu.NONE, android.R.id.cut, 0, android.R.string.cut);
        if ((activity.editActions & (1 << EDIT_COPY)) != 0) menu.add(Menu.NONE, android.R.id.copy, 1, android.R.string.copy);
        if ((activity.editActions & (1 << EDIT_PASTE)) != 0) menu.add(Menu.NONE, android.R.id.paste, 2, android.R.string.paste);
        if ((activity.editActions & (1 << EDIT_SELECT_ALL)) != 0) menu.add(Menu.NONE, android.R.id.selectAll, 3, android.R.string.selectAll);
        for (int i = 0; i < menu.size(); ++i) menu.getItem(i).setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
        return true;
    }
    @Override public boolean onActionItemClicked(ActionMode mode, MenuItem item) {
        int id = item.getItemId(), action = id == android.R.id.cut ? EDIT_CUT : id == android.R.id.copy ? EDIT_COPY
                                           : id == android.R.id.paste ? EDIT_PASTE : id == android.R.id.selectAll ? EDIT_SELECT_ALL : -1;
        if (action < 0) return false;
        nativeEvent(E_EDIT_ACTION, new int[] {action}, null, null);
        return true;
    }
    @Override public void onDestroyActionMode(ActionMode mode) { if (activity.editMode == mode) activity.editMode = null; }
    @Override public void onGetContentRect(ActionMode mode, View view, Rect out) { out.set(activity.editRect); }
}

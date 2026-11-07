package org.cascade;

import android.content.ClipData;
import android.content.ClipDescription;
import android.net.Uri;
import android.view.DragAndDropPermissions;
import android.view.DragEvent;
import android.view.View;
import java.util.ArrayList;
import java.util.List;

import static org.cascade.CascadeActivity.*;

/* What other apps drag over the page (split screen, free-form windows): its kinds while it is over,
 * its text and copies of its files at the drop. */
/* While a drag is over the page, its kinds; at the drop, its text and copies of its files. */
class CascadeDrops implements View.OnDragListener {
    final CascadeActivity activity;
    CascadeDrops(CascadeActivity a) { activity = a; }
    boolean inside;

    @Override public boolean onDrag(View v, DragEvent e) {
        switch (e.getAction()) {
        case DragEvent.ACTION_DRAG_STARTED: return true;
        case DragEvent.ACTION_DRAG_ENTERED: {
            inside = true;
            ClipDescription d = e.getClipDescription();
            boolean text = d != null && (d.hasMimeType("text/*") || d.hasMimeType(ClipDescription.MIMETYPE_TEXT_INTENT));
            nativeEvent(E_DRAG, new int[] {DRAG_ENTER, text ? 1 : 0}, new float[] {e.getX(), e.getY()}, null);
            return true;
        }
        case DragEvent.ACTION_DRAG_LOCATION:
            nativeEvent(E_DRAG, new int[] {DRAG_OVER, 0}, new float[] {e.getX(), e.getY()}, null);
            return true;
        case DragEvent.ACTION_DRAG_EXITED:
            inside = false;
            nativeEvent(E_DRAG, new int[] {DRAG_LEAVE, 0}, null, null);
            return true;
        case DragEvent.ACTION_DRAG_ENDED:
            if (inside) nativeEvent(E_DRAG, new int[] {DRAG_LEAVE, 0}, null, null);
            inside = false;
            return true;
        case DragEvent.ACTION_DROP: {
            inside = false;
            drop(e);
            return true;
        }
        default: return false;
        }
    }

    void drop(DragEvent e) {
        final float x = e.getX(), y = e.getY();
        final ClipData clip = e.getClipData();
        /* another app's URIs are readable while these permissions are held */
        final DragAndDropPermissions grant = clip == null ? null : activity.requestDragAndDropPermissions(e);
        final StringBuilder text = new StringBuilder();
        final List<Uri> uris = new ArrayList<Uri>();
        for (int k = 0; clip != null && k < clip.getItemCount(); ++k) {
            ClipData.Item it = clip.getItemAt(k);
            if (it.getUri() != null && "content".equals(it.getUri().getScheme())) uris.add(it.getUri());
            else if (it.getText() != null) text.append(text.length() > 0 ? "\n" : "").append(it.getText());
        }
        new Thread(new Runnable() {
            @Override public void run() {
                String paths = activity.copyIn(uris, "dropped");
                if (grant != null) grant.release();
                /* the text, a 0 byte, then the files' paths one per line */
                byte[] t = utf8(text.toString()), p = utf8(paths), all = new byte[t.length + 1 + p.length];
                System.arraycopy(t, 0, all, 0, t.length);
                System.arraycopy(p, 0, all, t.length + 1, p.length);
                nativeEvent(E_DRAG, new int[] {DRAG_DROP, 0}, new float[] {x, y}, all);
            }
        }).start();
    }
}

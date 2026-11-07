package org.cascade;

import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.Color;
import android.os.Build;
import android.view.View;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;

import static org.cascade.CascadeActivity.*;

/* The page's surroundings: the edge colors under the system's bars and around the notch. */
final class CascadeBackdrop extends FrameLayout {
    final CascadeActivity activity;
    int top, bottom;
    int[] left = new int[0], right = new int[0];
    final android.graphics.Paint paint = new android.graphics.Paint();
    CascadeBackdrop(CascadeActivity c) {
        super(c);
        activity = c;
        setWillNotDraw(false);
        /* (until the page's first frame, the theme's: no white flash before a dark page) */
        boolean dark = (c.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
        top = bottom = dark ? Color.BLACK : Color.WHITE;
    }
    void setColors(int t, int b, int[] l, int[] r) {
        top = t;
        bottom = b;
        left = l != null ? l : new int[0];
        right = r != null ? r : new int[0];
        invalidate();
        /* the bars' icons dark on a light page, light on a dark one */
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = activity.getWindow().getInsetsController();
            if (c != null) {
                int light = (light(t) ? WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS : 0)
                            | (light(b) ? WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS : 0);
                c.setSystemBarsAppearance(light, WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS);
            }
        } else activity.hideBars(activity.fullscreen);
    }
    @SuppressWarnings("deprecation")
    int lightBars() {
        return (light(top) ? View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR : 0) | (light(bottom) ? View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR : 0);
    }
    boolean light(int c) { return Color.red(c) * 299 + Color.green(c) * 587 + Color.blue(c) * 114 > 128000; }
    /* above the page its top row's color, below it its bottom row's; beside it (a notch, the bars
     * in landscape) each row continues the page's edge there */
    @Override protected void onDraw(Canvas canvas) {
        int w = getWidth(), h = getHeight();
        int l = getPaddingLeft(), t = getPaddingTop(), r = w - getPaddingRight(), b = h - getPaddingBottom();
        paint.setColor(top);
        canvas.drawRect(0, 0, w, t, paint);
        paint.setColor(bottom);
        canvas.drawRect(0, b, w, h, paint);
        side(canvas, left, 0, l, t, b);
        side(canvas, right, r, w, t, b);
    }
    void side(Canvas canvas, int[] runs, int x0, int x1, int t, int b) {
        if (x1 <= x0) return;
        if (runs.length < 2) {
            paint.setColor(top);
            canvas.drawRect(x0, t, x1, b, paint);
            return;
        }
        for (int i = 0; i + 1 < runs.length; i += 2) {
            int y0 = t + runs[i], y1 = i + 2 < runs.length ? Math.min(b, t + runs[i + 2]) : b; /* (the last run to the page's bottom) */
            if (y0 >= b) break;
            paint.setColor(runs[i + 1]);
            canvas.drawRect(x0, y0, x1, y1, paint);
        }
    }
}

/* The Android side of a cascade app: the activity that holds the page's surface and hands the C side
 * (src/platform/windowAndroid.c) everything the system offers only to Java. The program's main runs
 * on a thread of its own in C; this class runs on the system's main thread and only passes messages:
 * touch, mouse and keys; the input method, with the focused field's whole text (composition,
 * corrections, every language); the system's bars, notch and keyboard (the page is laid out between
 * them, the backdrop around it takes the page's edge colors); its theme, text size and animation
 * scale; the clipboard (text, pictures through CascadeFiles, files); what other apps drag in; the
 * floating edit menu and the magnifier over a selection made by touch; links (the web, mail, phone,
 * maps: never files or intents), sharing, the file picker and vibration; TalkBack, from a copy of
 * the page's accessible tree the C side sends; and the activity's life (background, foreground,
 * memory). Other apps' files are copied into the cache, with safe names; an earlier run's copies
 * are removed when the activity starts.
 *
 * Strings cross as UTF-8 bytes, offsets into text as byte offsets: Java's UTF-16 indices are turned
 * into them here. The library loaded is the one the manifest names (android.app.lib_name), as for a
 * NativeActivity.
 *
 * Its parts, each in a file of its own: CascadePageView (the surface, touch and keys),
 * CascadeInputConnection (the keyboard's edits), CascadeBackdrop (around the page), CascadeEditMenu
 * (the floating edit menu), CascadeDrops (what other apps drag in), CascadeAccessibility (TalkBack) and
 * CascadeFiles (the pictures it puts on the clipboard). */
package org.cascade;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipDescription;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.text.Editable;
import android.text.Selection;
import android.util.Log;
import android.text.SpannableStringBuilder;
import android.view.ActionMode;
import android.view.DisplayCutout;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.InputMethodManager;
import android.webkit.MimeTypeMap;
import android.widget.FrameLayout;
import android.widget.Magnifier;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

public class CascadeActivity extends Activity {
    /* what crosses to C (windowAndroid.c has the same numbers) */
    static final int E_TOUCH = 1, E_MOUSE = 2, E_KEY = 3, E_TEXT_STATE = 4, E_EDITOR_ACTION = 5, E_FOCUS = 6, E_APP = 7,
                     E_PREFERENCES = 8, E_BACK = 9, E_FILES = 10, E_A11Y_ON = 11, E_A11Y_ACTION = 12, E_DESTROY = 13,
                     E_EDIT_ACTION = 14, E_DRAG = 15, E_SAVE = 16;
    /* CscdEditAction (view.h); what E_DRAG says */
    static final int EDIT_CUT = 0, EDIT_COPY = 1, EDIT_PASTE = 2, EDIT_SELECT_ALL = 3;
    static final int DRAG_ENTER = 0, DRAG_OVER = 1, DRAG_LEAVE = 2, DRAG_DROP = 3;
    static final int S_CREATED = 1, S_CHANGED = 2, S_DESTROYED = 3;
    static final int APP_BACKGROUND = 0, APP_FOREGROUND = 1, APP_LOW_MEMORY = 2;
    /* CscdInputKind, CscdEnterKey, CscdCapitalize (view.h) */
    static final int K_NONE = 0, K_TEXT = 1, K_MULTILINE = 2, K_NUMBER = 3, K_DECIMAL = 4, K_TEL = 5, K_EMAIL = 6, K_URL = 7,
                     K_SEARCH = 8, K_PASSWORD = 9;
    static final int ENTER_DEFAULT = 0, ENTER_ENTER = 1, ENTER_DONE = 2, ENTER_GO = 3, ENTER_NEXT = 4, ENTER_PREVIOUS = 5,
                     ENTER_SEARCH = 6, ENTER_SEND = 7;
    /* CscdAccessibleAction, CscdRole and the CSCD_A11Y_ state bits (view.h) */
    static final int A_ACTIVATE = 0, A_FOCUS = 1, A_SET_VALUE = 2, A_SCROLL_INTO_VIEW = 3;
    static final int ST_FOCUSABLE = 1, ST_FOCUSED = 2, ST_CHECKED = 4, ST_MIXED = 8, ST_DISABLED = 16, ST_EXPANDED = 32,
                     ST_COLLAPSED = 64, ST_SELECTED = 128, ST_READONLY = 256, ST_REQUIRED = 512, ST_MULTILINE = 1024, ST_PASSWORD = 2048;
    static final int PICK_FILES = 0x5e1f, SAVE_FILE = 0x5e20;

    static native void nativeCreate(Activity activity, Object assets, String internalPath);
    static native void nativeSurface(int kind, Surface surface, int width, int height);
    static native void nativeEvent(int kind, int[] ints, float[] floats, byte[] text);

    CascadePageView page;
    CascadeBackdrop root;
    InputMethodManager imm;
    AccessibilityManager a11y;

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        String lib = "main";
        try {
            ActivityInfo ai = getPackageManager().getActivityInfo(getComponentName(), PackageManager.GET_META_DATA);
            if (ai.metaData != null && ai.metaData.getString("android.app.lib_name") != null) lib = ai.metaData.getString("android.app.lib_name");
        } catch (PackageManager.NameNotFoundException e) {
            /* (the default name) */
        }
        System.loadLibrary(lib);
        imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
        a11y = (AccessibilityManager) getSystemService(Context.ACCESSIBILITY_SERVICE);

        /* the page between the system's bars, notch and keyboard; what is around it, the backdrop */
        Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        window.setStatusBarColor(Color.TRANSPARENT);
        window.setNavigationBarColor(Color.TRANSPARENT);
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        if (Build.VERSION.SDK_INT >= 30) window.setDecorFitsSystemWindows(false);
        else window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                                                         | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        if (Build.VERSION.SDK_INT >= 28) window.getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        root = new CascadeBackdrop(this);
        page = new CascadePageView(this);
        root.addView(page, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        root.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
                int l, t, r, b;
                if (Build.VERSION.SDK_INT >= 30) {
                    android.graphics.Insets i = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
                    l = i.left; t = i.top; r = i.right; b = i.bottom;
                } else {
                    l = insets.getSystemWindowInsetLeft(); t = insets.getSystemWindowInsetTop();
                    r = insets.getSystemWindowInsetRight(); b = insets.getSystemWindowInsetBottom();
                    if (Build.VERSION.SDK_INT >= 28 && insets.getDisplayCutout() != null) {
                        DisplayCutout c = insets.getDisplayCutout();
                        l = Math.max(l, c.getSafeInsetLeft()); t = Math.max(t, c.getSafeInsetTop());
                        r = Math.max(r, c.getSafeInsetRight()); b = Math.max(b, c.getSafeInsetBottom());
                    }
                }
                if (fullscreen) l = t = r = 0; /* (the bars are hidden; the keyboard still takes the bottom) */
                if (fullscreen && b < v.getHeight() / 5) b = 0;
                v.setPadding(l, t, r, b);
                return insets;
            }
        });
        setContentView(root);
        page.setFocusable(true);
        page.setFocusableInTouchMode(true);
        page.requestFocus();
        page.setOnDragListener(new CascadeDrops(this));
        forgetCopies();
        /* Back as Android 13 and later deliver it (and every app targeting Android 16 gets it only this
         * way); onBackPressed below is the older way */
        if (Build.VERSION.SDK_INT >= 33)
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                new android.window.OnBackInvokedCallback() {
                    @Override public void onBackInvoked() { nativeEvent(E_BACK, null, null, null); }
                });
        nativeCreate(this, getAssets(), getFilesDir().getAbsolutePath());
        sendPreferences();
        a11y.addAccessibilityStateChangeListener(new AccessibilityManager.AccessibilityStateChangeListener() {
            @Override public void onAccessibilityStateChanged(boolean on) { nativeEvent(E_A11Y_ON, new int[] {on ? 1 : 0}, null, null); }
        });
        nativeEvent(E_A11Y_ON, new int[] {a11y.isEnabled() ? 1 : 0}, null, null);
    }

    @Override protected void onStart() { super.onStart(); nativeEvent(E_APP, new int[] {APP_FOREGROUND}, null, null); }
    @Override protected void onResume() { super.onResume(); sendPreferences(); /* (a setting may have changed meanwhile) */ }
    @Override protected void onStop() { nativeEvent(E_APP, new int[] {APP_BACKGROUND}, null, null); super.onStop(); }
    @Override public void onLowMemory() { super.onLowMemory(); nativeEvent(E_APP, new int[] {APP_LOW_MEMORY}, null, null); }
    @Override public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        if (level == TRIM_MEMORY_RUNNING_LOW || level == TRIM_MEMORY_RUNNING_CRITICAL || level >= TRIM_MEMORY_MODERATE)
            nativeEvent(E_APP, new int[] {APP_LOW_MEMORY}, null, null);
    }
    @Override protected void onDestroy() { nativeEvent(E_DESTROY, null, null, null); super.onDestroy(); }
    @Override public void onConfigurationChanged(Configuration c) { super.onConfigurationChanged(c); sendPreferences(); }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        nativeEvent(E_FOCUS, new int[] {focused ? 1 : 0}, null, null);
        if (focused && fullscreen) hideBars(true);
    }
    @SuppressWarnings("deprecation")
    @Override public void onBackPressed() { nativeEvent(E_BACK, null, null, null); }

    /* the theme, the animations' scale (0: off in the developer or accessibility settings), the text
     * size and the density */
    void sendPreferences() {
        Configuration c = getResources().getConfiguration();
        boolean dark = (c.uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
        float animations = Settings.Global.getFloat(getContentResolver(), Settings.Global.ANIMATOR_DURATION_SCALE, 1f);
        float density = getResources().getDisplayMetrics().density;
        nativeEvent(E_PREFERENCES, new int[] {dark ? 1 : 0, animations == 0 ? 1 : 0}, new float[] {c.fontScale, density}, null);
    }

    static byte[] utf8(String s) { return s == null ? new byte[0] : s.getBytes(StandardCharsets.UTF_8); }
    static String string(byte[] b) { return b == null ? "" : new String(b, StandardCharsets.UTF_8); }
    /* UTF-16 index -> UTF-8 byte offset, and back */
    static int bytesBefore(CharSequence s, int index) {
        int n = 0;
        index = Math.max(0, Math.min(index, s.length()));
        for (int i = 0; i < index; ++i) {
            char c = s.charAt(i);
            if (c < 0x80) n += 1;
            else if (c < 0x800) n += 2;
            else if (Character.isHighSurrogate(c) && i + 1 < index) { n += 4; ++i; }
            else n += 3;
        }
        return n;
    }
    static int indexAt(CharSequence s, int bytes) {
        int n = 0, i = 0;
        for (; i < s.length() && n < bytes; ++i) {
            char c = s.charAt(i);
            if (c < 0x80) n += 1;
            else if (c < 0x800) n += 2;
            else if (Character.isHighSurrogate(c) && i + 1 < s.length()) { n += 4; ++i; }
            else n += 3;
        }
        return i;
    }

    /* ---- called from the C side's thread ---------------------------------------------------------------- */

    boolean fullscreen;

    void setFullscreen(final boolean on) {
        runOnUiThread(new Runnable() { @Override public void run() { fullscreen = on; hideBars(on); root.requestApplyInsets(); } });
    }

    @SuppressWarnings("deprecation")
    void hideBars(boolean hide) {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c == null) return;
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            if (hide) c.hide(WindowInsets.Type.systemBars());
            else c.show(WindowInsets.Type.systemBars());
        } else {
            int flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION;
            if (hide) flags |= View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
            getWindow().getDecorView().setSystemUiVisibility(flags | root.lightBars());
        }
    }

    /* the page's edge colors: its top and bottom rows', and its left and right columns' as (first
     * row, color) runs, rows in the page view's pixels */
    void setBackdrop(final int top, final int bottom, final int[] left, final int[] right) {
        runOnUiThread(new Runnable() { @Override public void run() { root.setColors(top, bottom, left, right); } });
    }

    /* where a drag sideways is the page's (left, top, right, bottom in the page's pixels, in fours): the
     * back gesture from the screen's sides leaves it alone (Android 10 and later) */
    void setGestureExclusion(final int[] r) {
        if (Build.VERSION.SDK_INT < 29) return;
        runOnUiThread(new Runnable() { @Override public void run() {
            ArrayList<Rect> list = new ArrayList<>();
            for (int i = 0; i + 3 < r.length; i += 4) list.add(new Rect(r[i], r[i + 1], r[i + 2], r[i + 3]));
            page.setSystemGestureExclusionRects(list);
        } });
    }

    void finishApp() { runOnUiThread(new Runnable() { @Override public void run() { finish(); } }); }

    void setClipboard(byte[] text) {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm != null) cm.setPrimaryClip(ClipData.newPlainText("text", string(text)));
    }

    byte[] getClipboard() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm == null || !cm.hasPrimaryClip()) return new byte[0];
        ClipData clip = cm.getPrimaryClip();
        if (clip == null || clip.getItemCount() == 0) return new byte[0];
        ClipData.Item item = clip.getItemAt(0);
        if (item.getText() != null) return utf8(item.getText().toString());
        /* a URI's contents only when they are text (a picture's bytes are not read as text); a web link as itself */
        ClipDescription d = clip.getDescription();
        Uri u = item.getUri();
        if (u != null && (d == null || !d.hasMimeType("text/*"))) {
            String s = u.getScheme();
            return utf8("http".equals(s) || "https".equals(s) ? u.toString() : "");
        }
        CharSequence t = item.coerceToText(this);
        return utf8(t == null ? "" : t.toString());
    }

    /* A picture (PNG bytes) to the clipboard: a file in the cache's shared folder, which apps that
     * paste read by its content:// URI (CascadeFiles; without it in the manifest, nothing). */
    void setClipboardImage(byte[] png) {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm == null || png == null || getPackageManager().resolveContentProvider(CascadeFiles.authority(this), 0) == null) return;
        File dir = CascadeFiles.folder(this);
        dir.mkdirs();
        File[] old = dir.listFiles(); /* (the picture copied before is not the clipboard's any more) */
        if (old != null) for (File f : old) if (f.getName().startsWith("copied-")) f.delete();
        File f = new File(dir, "copied-" + System.currentTimeMillis() + ".png");
        try (OutputStream out = new FileOutputStream(f)) {
            out.write(png);
        } catch (Exception e) {
            return;
        }
        cm.setPrimaryClip(ClipData.newUri(getContentResolver(), "image", CascadeFiles.uriFor(this, f)));
    }

    /* What the clipboard holds as files (a picture, a document another app copied): copies in the
     * cache, their paths one per line. */
    byte[] getClipboardFiles() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        ClipData clip = cm == null || !cm.hasPrimaryClip() ? null : cm.getPrimaryClip();
        if (clip == null) return new byte[0];
        List<Uri> uris = new ArrayList<Uri>();
        for (int k = 0; k < clip.getItemCount(); ++k) if (clip.getItemAt(k).getUri() != null) uris.add(clip.getItemAt(k).getUri());
        return utf8(copyIn(uris, "pasted"));
    }

    /* Links the system opens: the web, mail, phone, messages, maps, the store; never files, other
     * apps' content, intents or scripts, which a page's link must not reach. */
    static boolean openable(Uri u) {
        String s = u.getScheme();
        if (s == null) return false;
        s = s.toLowerCase();
        return s.equals("http") || s.equals("https") || s.equals("mailto") || s.equals("tel") || s.equals("sms") || s.equals("smsto")
            || s.equals("geo") || s.equals("market");
    }

    boolean openUrl(byte[] url) {
        try {
            Uri u = Uri.parse(string(url).trim());
            if (!openable(u)) return false;
            Intent i = new Intent(Intent.ACTION_VIEW, u);
            i.addCategory(Intent.CATEGORY_BROWSABLE);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(i);
            return true;
        } catch (RuntimeException e) { /* (no app for it, or refused) */
            return false;
        }
    }

    boolean share(byte[] text) {
        try {
            Intent i = new Intent(Intent.ACTION_SEND);
            i.setType("text/plain");
            i.putExtra(Intent.EXTRA_TEXT, string(text));
            Intent chooser = Intent.createChooser(i, null);
            chooser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(chooser);
            return true;
        } catch (ActivityNotFoundException e) {
            return false;
        }
    }

    void haptic(final int kind) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                int c = HapticFeedbackConstants.VIRTUAL_KEY;
                if (kind == 1) c = HapticFeedbackConstants.CLOCK_TICK;
                else if (kind == 2 && Build.VERSION.SDK_INT >= 30) c = HapticFeedbackConstants.CONFIRM;
                else if ((kind == 3 || kind == 4) && Build.VERSION.SDK_INT >= 30) c = HapticFeedbackConstants.REJECT;
                else if (kind == 3 || kind == 4) c = HapticFeedbackConstants.LONG_PRESS;
                page.performHapticFeedback(c);
            }
        });
    }

    /* ---- touch selection: the system's floating edit menu and magnifier -------------------------------- */

    ActionMode editMode;
    final Rect editRect = new Rect();
    int editActions;
    Magnifier magnifier;

    /* show: next to rect (the page view's pixels), with the actions (CSCD_EDIT_BIT()s) it offers */
    void showEditMenu(final boolean show, final int l, final int t, final int r, final int b, final int actions) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (!show) {
                    if (editMode != null) { ActionMode m = editMode; editMode = null; m.finish(); }
                    return;
                }
                editRect.set(l, t, r, b);
                boolean same = actions == editActions;
                editActions = actions;
                if (editMode != null) {
                    if (!same) editMode.invalidate();
                    editMode.invalidateContentRect();
                    return;
                }
                editMode = page.startActionMode(new CascadeEditMenu(CascadeActivity.this), ActionMode.TYPE_FLOATING);
            }
        });
    }


    /* the magnifier over a dragged selection handle (Android 9 and later) */
    void magnify(final boolean show, final float x, final float y) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (Build.VERSION.SDK_INT < 28) return;
                try {
                    if (!show) {
                        if (magnifier != null) magnifier.dismiss();
                        return;
                    }
                    if (magnifier == null) magnifier = new Magnifier(page);
                    magnifier.show(x, y);
                } catch (RuntimeException e) {
                    magnifier = null; /* (a surface it cannot copy from) */
                }
            }
        });
    }

    /* ---- other apps' files: picked, pasted and dropped (CascadeDrops), copied into the cache ----------- */

    /* The file picker (the system's documents UI): the files chosen are copied into the cache, where
     * the C side can read them by path. */
    boolean pickFiles(byte[] acceptBytes, final boolean multiple) {
        final String accept = string(acceptBytes);
        runOnUiThread(new Runnable() {
            @Override public void run() {
                List<String> types = new ArrayList<String>();
                for (String part : accept.split(",")) {
                    String p = part.trim().toLowerCase();
                    if (p.isEmpty()) continue;
                    if (p.startsWith(".")) {
                        String t = MimeTypeMap.getSingleton().getMimeTypeFromExtension(p.substring(1));
                        if (t != null && !types.contains(t)) types.add(t);
                    } else if (!types.contains(p)) types.add(p);
                }
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType(types.size() == 1 ? types.get(0) : "*/*");
                if (types.size() > 1) i.putExtra(Intent.EXTRA_MIME_TYPES, types.toArray(new String[0]));
                i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multiple);
                try {
                    startActivityForResult(i, PICK_FILES);
                } catch (ActivityNotFoundException e) {
                    nativeEvent(E_FILES, new int[] {0}, null, null);
                }
            }
        });
        return true;
    }

    /* Saving (cscdWindowSaveFile): the documents UI makes the document; the C side writes a file in
     * the cache (E_SAVE), and savedFile copies it into the document. */
    Uri saveTo;
    File saving;

    boolean saveFile(byte[] nameBytes, byte[] acceptBytes) {
        final String name = string(nameBytes), accept = string(acceptBytes);
        runOnUiThread(new Runnable() {
            @Override public void run() {
                String type = null;
                String first = accept.split(",")[0].trim().toLowerCase();
                if (first.startsWith(".")) type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(first.substring(1));
                else if (!first.isEmpty() && !first.endsWith("*")) type = first;
                Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType(type != null ? type : "application/octet-stream");
                if (!name.isEmpty()) i.putExtra(Intent.EXTRA_TITLE, name);
                try {
                    startActivityForResult(i, SAVE_FILE);
                } catch (ActivityNotFoundException e) {
                    nativeEvent(E_SAVE, new int[] {0}, null, null);
                }
            }
        });
        return true;
    }

    /* (the program's thread, after the C side wrote, or did not write, the file) */
    void savedFile(final boolean wrote) {
        final Uri to = saveTo;
        final File from = saving;
        saveTo = null;
        saving = null;
        if (to == null || from == null) return;
        new Thread(new Runnable() {
            @Override public void run() {
                if (wrote) {
                    try (InputStream in = new FileInputStream(from); OutputStream out = getContentResolver().openOutputStream(to, "wt")) {
                        byte[] buf = new byte[65536];
                        for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
                    } catch (IOException | RuntimeException e) {
                        Log.w("cascade", "saving " + to + ": " + e);
                    }
                } else {
                    try {
                        DocumentsContract.deleteDocument(getContentResolver(), to); /* (nothing written: no empty document left) */
                    } catch (Exception e) {
                        /* (a provider that cannot: it stays empty) */
                    }
                }
                from.delete();
                from.getParentFile().delete();
            }
        }).start();
    }

    @Override protected void onActivityResult(int request, int result, final Intent data) {
        super.onActivityResult(request, result, data);
        if (request == SAVE_FILE) {
            Uri u = result == RESULT_OK && data != null ? data.getData() : null;
            String path = null;
            if (u != null) {
                String name = null;
                try (Cursor c = getContentResolver().query(u, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                    if (c != null && c.moveToFirst()) name = c.getString(0);
                } catch (RuntimeException e) {
                    /* (a name of our own) */
                }
                if (name == null || name.isEmpty() || name.contains("/") || name.startsWith(".")) name = "file";
                File dir = new File(getCacheDir(), "saving/" + System.nanoTime());
                if (dir.mkdirs()) {
                    saveTo = u;
                    saving = new File(dir, name);
                    path = saving.getPath();
                }
            }
            nativeEvent(E_SAVE, new int[] {path != null ? 1 : 0}, null, path != null ? utf8(path) : null);
            return;
        }
        if (request != PICK_FILES) return;
        final List<Uri> uris = new ArrayList<Uri>();
        if (result == RESULT_OK && data != null) {
            if (data.getClipData() != null)
                for (int k = 0; k < data.getClipData().getItemCount(); ++k) uris.add(data.getClipData().getItemAt(k).getUri());
            else if (data.getData() != null) uris.add(data.getData());
        }
        new Thread(new Runnable() {
            @Override public void run() { nativeEvent(E_FILES, new int[] {1}, null, utf8(copyIn(uris, "picked"))); }
        }).start();
    }

    /* Other apps' files (content:// URIs only) copied into the cache's folder, where the C side reads
     * them by path; their paths, one per line. Each batch has a folder of its own, and each file its
     * name, made safe (no path in it, nothing hidden) and unique in the folder. */
    String copyIn(List<Uri> uris, String folder) {
        StringBuilder paths = new StringBuilder();
        File dir = new File(getCacheDir(), folder + "/" + System.nanoTime());
        if (uris.isEmpty() || !dir.mkdirs()) return "";
        for (Uri u : uris) {
            if (u == null || !"content".equals(u.getScheme())) continue;
            String name = null, type = null;
            try (Cursor c = getContentResolver().query(u, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (c != null && c.moveToFirst()) name = c.getString(0);
                type = getContentResolver().getType(u);
            } catch (RuntimeException e) {
                /* (a provider that does not answer: a name of our own) */
            }
            File f = new File(dir, uniqueName(dir, safeName(name, type)));
            boolean ok = false;
            try (InputStream in = getContentResolver().openInputStream(u); OutputStream out = new FileOutputStream(f)) {
                if (in != null) {
                    byte[] buf = new byte[65536];
                    int n;
                    while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                    ok = true;
                }
            } catch (Exception e) {
                ok = false;
            }
            if (!ok) { f.delete(); continue; }
            if (paths.length() > 0) paths.append('\n');
            paths.append(f.getAbsolutePath());
        }
        return paths.toString();
    }

    static String safeName(String name, String type) {
        StringBuilder s = new StringBuilder();
        if (name != null)
            for (int i = 0; i < name.length() && s.length() < 120; ++i) {
                char c = name.charAt(i);
                s.append(c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':' ? '_' : c);
            }
        while (s.length() > 0 && (s.charAt(0) == '.' || s.charAt(0) == ' ')) s.deleteCharAt(0);
        if (s.length() == 0) {
            String ext = type == null ? null : MimeTypeMap.getSingleton().getExtensionFromMimeType(type);
            s.append("file").append(ext != null ? "." + ext : "");
        }
        return s.toString();
    }

    static String uniqueName(File dir, String name) {
        if (!new File(dir, name).exists()) return name;
        int dot = name.lastIndexOf('.');
        String stem = dot > 0 ? name.substring(0, dot) : name, ext = dot > 0 ? name.substring(dot) : "";
        for (int k = 2;; ++k)
            if (!new File(dir, stem + " (" + k + ")" + ext).exists()) return stem + " (" + k + ")" + ext;
    }

    /* the copies an earlier run made (picked, pasted, dropped, copied out): the cache is not left to
     * fill up with them */
    void forgetCopies() {
        final File cache = getCacheDir();
        new Thread(new Runnable() {
            @Override public void run() {
                /* (not "shared": the clipboard may still hold the picture last copied) */
                for (String folder : new String[] {"picked", "pasted", "dropped"}) removeTree(new File(cache, folder));
            }
        }).start();
    }

    static void removeTree(File f) {
        File[] kids = f.isDirectory() ? f.listFiles() : null;
        if (kids != null) for (File k : kids) removeTree(k);
        f.delete();
    }

    /* ---- the input method ----------------------------------------------------------------------------------- */

    int inputKind = K_NONE, enterKey, capitalize;
    boolean autocorrect;
    final Editable editable = new SpannableStringBuilder();

    /* the keyboard the focused field asks for (kind NONE: none) */
    void setInput(final int kind, final int enter, final int cap, final boolean correct) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                boolean changed = kind != inputKind || enter != enterKey || cap != capitalize || correct != autocorrect;
                inputKind = kind;
                enterKey = enter;
                capitalize = cap;
                autocorrect = correct;
                if (kind == K_NONE) {
                    imm.hideSoftInputFromWindow(page.getWindowToken(), 0);
                    return;
                }
                if (changed) imm.restartInput(page);
                page.requestFocus();
                imm.showSoftInput(page, 0);
            }
        });
    }

    /* the field's text as the C side has it now (changed by keys, touches or the program) */
    void setTextState(final byte[] text, final int selStart, final int selEnd, final int compStart, final int compEnd) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                String s = string(text);
                boolean textChanged = !s.equals(editable.toString());
                if (textChanged) editable.replace(0, editable.length(), s);
                int a = indexAt(s, selStart), z = indexAt(s, selEnd);
                Selection.setSelection(editable, a, z);
                int ca = -1, cz = -1;
                if (compEnd > compStart) {
                    ca = indexAt(s, compStart);
                    cz = indexAt(s, compEnd);
                    BaseInputConnection.removeComposingSpans(editable);
                    editable.setSpan(composing, ca, cz, android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE | android.text.Spanned.SPAN_COMPOSING);
                } else BaseInputConnection.removeComposingSpans(editable);
                if (inputKind == K_NONE) return;
                if (textChanged && (connection == null || connection.batch == 0)) imm.restartInput(page);
                else imm.updateSelection(page, a, z, ca, cz);
            }
        });
    }

    final Object composing = new android.text.style.UnderlineSpan();
    CascadeInputConnection connection;


    void sendKey(KeyEvent e) {
        int unicode = e.getUnicodeChar(e.getMetaState());
        nativeEvent(E_KEY, new int[] {e.getAction(), e.getKeyCode(), e.getMetaState(), e.getRepeatCount(), unicode}, null,
                    e.getAction() == KeyEvent.ACTION_MULTIPLE && e.getCharacters() != null ? utf8(e.getCharacters()) : null);
    }

    /* ---- TalkBack (CascadeAccessibility) ------------------------------------------------------------------- */

    final CascadeAccessibility tree = new CascadeAccessibility(this);

    /* the page's accessible elements as the C side sends them: per element 10 ints (id, parent, role,
     * state, level, left, top, right, bottom, range) and 4 floats (value, min, max, step), and its name,
     * description and text, each ended by a 0 byte */
    void setAccessibleTree(final int[] ints, final float[] floats, final byte[] strings) {
        runOnUiThread(new Runnable() { @Override public void run() { tree.load(ints, floats, strings); } });
    }

    /* kind: 0 focus, 1 value, 2 state, 3 structure. Focus is told at once; changes are gathered and
     * told every 100 ms at most, once per element (as Android's own views do): a value that changes
     * each frame (a progress bar) does not flood TalkBack. */
    final java.util.HashSet<Integer> changed = new java.util.HashSet<Integer>();
    boolean changesPosted;
    final Runnable tellChanges = new Runnable() {
        @Override public void run() {
            changesPosted = false;
            if (changed.contains(View.NO_ID)) tree.send(View.NO_ID, AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
            else for (int id : changed) tree.send(id, AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
            changed.clear();
        }
    };

    void accessibleEvent(final int kind, final int id) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (kind == 0) {
                    tree.send(id, AccessibilityEvent.TYPE_VIEW_FOCUSED);
                    return;
                }
                changed.add(kind == 3 ? View.NO_ID : id);
                if (!changesPosted) {
                    changesPosted = true;
                    page.postDelayed(tellChanges, 100);
                }
            }
        });
    }

}

package io.github.joeyballentine.bfbb;

import android.Manifest;
import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Copies the game's files from a folder the player picks into the app's own
 * assets directory, where iAndroidStartup points BFBB_ASSETS.
 *
 * <p>The folder is chosen through the system picker, so it can be anywhere the
 * device can see without a storage permission. What is copied is what the
 * game reads, from an extracted Xbox disc: the .HIP and .HOP archives at the
 * top and in the two-letter level folders, the movies in fmv/, and sb.ini.
 * Everything else in the folder -- the .xbe, extraction leftovers -- stays
 * behind.
 *
 * <p>Or the folder is used where it is: with All files access granted, the
 * picked folder's path is kept and BfbbActivity passes it to the game as
 * BFBB_ASSETS, so nothing is copied. That permission is not one Google Play
 * accepts for a game, which is why copying is the other choice.
 *
 * <p>The copy goes to a temporary folder that replaces assets/ only once it is
 * complete, so an interrupted import never leaves half of a set. The previous
 * files survive it too, unless there was room for only one copy of the game,
 * in which case they are deleted before the copy starts.
 */
public class ImportActivity extends Activity {

    private static final String TAG = "bfbb";
    private static final int PICK_FOLDER = 1;
    private static final int PICK_LINK = 2;

    static final String KEY_ASSETS_PATH = "assets_path";

    private boolean mAwaitingPermission;
    private Button mLink;

    private TextView mStatus;
    private ProgressBar mProgress;
    private Button mChoose;

    private static volatile Thread sCopy;

    /** Whether a usable set of game files is already in place. */
    static boolean assetsPresent(Activity activity) {
        if (linkedPath(activity) != null) {
            return true;
        }
        File external = activity.getExternalFilesDir(null);
        if (external == null) {
            return false;
        }
        return containsGame(new File(external, "assets"));
    }

    /**
     * The folder the player chose to use in place, while it still holds the
     * game and the app can still read it. Null otherwise, which sends the
     * next launch back to this screen.
     */
    static String linkedPath(Context context) {
        String path = context.getSharedPreferences(ProfileActivity.PREFS, MODE_PRIVATE)
                .getString(KEY_ASSETS_PATH, null);
        if (path == null || !canReadStorage(context) || !containsGame(new File(path))) {
            return null;
        }
        return path;
    }

    private static boolean canReadStorage(Context context) {
        if (Build.VERSION.SDK_INT >= 30) {
            return Environment.isExternalStorageManager();
        }
        return context.checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
    }

    private static boolean containsGame(File dir) {
        String[] names = dir.list();
        if (names == null) {
            return false;
        }
        boolean boot = false;
        boolean font = false;
        for (String n : names) {
            boot |= n.equalsIgnoreCase("boot.hip");
            font |= n.equalsIgnoreCase("font.hip");
        }
        return boot && font;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        int pad = (int) (24 * getResources().getDisplayMetrics().density);

        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText(R.string.import_title);
        title.setTextSize(24);
        title.setGravity(Gravity.CENTER);
        layout.addView(title);

        mStatus = new TextView(this);
        mStatus.setText(R.string.import_explain);
        mStatus.setTextSize(16);
        mStatus.setGravity(Gravity.CENTER);
        mStatus.setPadding(0, pad, 0, pad);
        layout.addView(mStatus);

        mProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        mProgress.setMax(1000);
        mProgress.setVisibility(View.GONE);
        layout.addView(mProgress, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        mChoose = new Button(this);
        mChoose.setText(R.string.import_choose);
        mChoose.setOnClickListener(v -> startActivityForResult(
                new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER));
        layout.addView(mChoose);

        mLink = new Button(this);
        mLink.setText(R.string.link_choose);
        mLink.setOnClickListener(v -> startLink());
        layout.addView(mLink);

        setContentView(layout);

        if (sCopy != null) {
            showBusy(getString(R.string.import_copying));
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        if (requestCode == PICK_FOLDER) {
            startCopy(data.getData());
        } else if (requestCode == PICK_LINK) {
            linkFolder(data.getData());
        }
    }

    // Coming back from the settings page, or from the permission dialog on an
    // Android too old to have All files access.
    @Override
    protected void onResume() {
        super.onResume();
        if (!mAwaitingPermission) {
            return;
        }
        mAwaitingPermission = false;
        if (canReadStorage(this)) {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_LINK);
        } else {
            mStatus.setText(R.string.link_no_permission);
        }
    }

    private void startLink() {
        if (canReadStorage(this)) {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_LINK);
            return;
        }

        mAwaitingPermission = true;
        if (Build.VERSION.SDK_INT >= 30) {
            mStatus.setText(R.string.link_permission);
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (ActivityNotFoundException e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE }, 1);
        }
    }

    private void linkFolder(Uri tree) {
        String path = treeToPath(tree);
        if (path == null) {
            showIdle(getString(R.string.link_not_local));
            return;
        }

        File root = findGameDir(new File(path));
        if (root == null) {
            showIdle(getString(R.string.import_not_found));
            return;
        }

        getSharedPreferences(ProfileActivity.PREFS, MODE_PRIVATE).edit()
                .putString(KEY_ASSETS_PATH, root.getPath()).apply();

        // A copy made earlier is no longer read, and is most of the space the
        // app takes up.
        File external = getExternalFilesDir(null);
        if (external != null) {
            final File copied = new File(external, "assets");
            new Thread(() -> deleteTree(copied)).start();
        }

        startActivity(new Intent(this, ProfileActivity.class));
        finish();
    }

    /**
     * A picked folder's path on disk, from the external storage provider's
     * document id: "primary:Download/BFBB" is the device's own storage and
     * "1A2B-3C4D:BFBB" an SD card. Null for any other provider, whose folders
     * are not files the game can open.
     */
    private static String treeToPath(Uri tree) {
        if (!"com.android.externalstorage.documents".equals(tree.getAuthority())) {
            return null;
        }
        String id = DocumentsContract.getTreeDocumentId(tree);
        int colon = id.indexOf(':');
        if (colon < 0) {
            return null;
        }
        String volume = id.substring(0, colon);
        String rel = id.substring(colon + 1);
        String base = "primary".equalsIgnoreCase(volume)
                ? Environment.getExternalStorageDirectory().getPath()
                : "/storage/" + volume;
        return rel.isEmpty() ? base : base + "/" + rel;
    }

    private static File findGameDir(File dir) {
        if (containsGame(dir)) {
            return dir;
        }
        File[] kids = dir.listFiles();
        if (kids != null) {
            for (File k : kids) {
                if (k.isDirectory() && containsGame(k)) {
                    return k;
                }
            }
        }
        return null;
    }

    private void showBusy(String status) {
        mChoose.setEnabled(false);
        mLink.setEnabled(false);
        mProgress.setVisibility(View.VISIBLE);
        mStatus.setText(status);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    private void showIdle(String status) {
        mChoose.setEnabled(true);
        mLink.setEnabled(true);
        mProgress.setVisibility(View.GONE);
        mStatus.setText(status);
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    private void startCopy(Uri tree) {
        File external = getExternalFilesDir(null);
        if (external == null) {
            showIdle(getString(R.string.import_no_storage));
            return;
        }

        showBusy(getString(R.string.import_scanning));
        ContentResolver resolver = getContentResolver();

        Thread t = new Thread(() -> {
            try {
                List<Entry> files = new ArrayList<>();
                String root = findGameRoot(resolver, tree, DocumentsContract.getTreeDocumentId(tree));
                if (root == null) {
                    runOnUiThread(() -> showIdle(getString(R.string.import_not_found)));
                    return;
                }
                collect(resolver, tree, root, "", true, files);

                long total = 0;
                for (Entry e : files) {
                    total += e.size;
                }

                File assets = new File(external, "assets");
                File staging = new File(external, "assets.importing");
                deleteTree(staging);

                // Room for the new set beside the old one, or failing that, in
                // the old one's place: it is about to be replaced either way,
                // and a phone with room for one copy of the game and not two is
                // the ordinary case.
                long free = new StatFs(external.getPath()).getAvailableBytes();
                if (free < total) {
                    final long withOld = free + treeSize(assets);
                    if (withOld < total) {
                        final long need = total;
                        runOnUiThread(() -> showIdle(getString(R.string.import_no_space,
                                gigabytes(need), gigabytes(withOld))));
                        return;
                    }
                    deleteTree(assets);
                }

                long done = 0;
                byte[] buffer = new byte[1 << 20];
                for (Entry e : files) {
                    File out = new File(staging, e.path);
                    File parent = out.getParentFile();
                    if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                        throw new IOException("could not create " + parent);
                    }
                    try (InputStream in = resolver.openInputStream(
                                 DocumentsContract.buildDocumentUriUsingTree(tree, e.id));
                         OutputStream o = new FileOutputStream(out)) {
                        if (in == null) {
                            throw new IOException("could not open " + e.path);
                        }
                        int n;
                        while ((n = in.read(buffer)) > 0) {
                            o.write(buffer, 0, n);
                            done += n;
                        }
                    }
                    final long d = done;
                    final long tot = total;
                    final String name = e.path;
                    runOnUiThread(() -> {
                        mProgress.setProgress(tot > 0 ? (int) (d * 1000 / tot) : 1000);
                        mStatus.setText(getString(R.string.import_progress, name,
                                gigabytes(d), gigabytes(tot)));
                    });
                }

                deleteTree(assets);
                if (!staging.renameTo(assets)) {
                    throw new IOException("could not move the files into " + assets);
                }

                runOnUiThread(() -> {
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    // The copy is what the game reads from now on.
                    getSharedPreferences(ProfileActivity.PREFS, MODE_PRIVATE).edit()
                            .remove(KEY_ASSETS_PATH).apply();
                    startActivity(new Intent(this, ProfileActivity.class));
                    finish();
                });
            } catch (IOException | RuntimeException e) {
                Log.e(TAG, "import failed", e);
                final String why = e.getMessage() != null ? e.getMessage() : e.toString();
                runOnUiThread(() -> showIdle(getString(R.string.import_failed, why)));
            } finally {
                sCopy = null;
            }
        });

        sCopy = t;
        t.start();
    }

    private static final class Entry {
        final String id;
        final String path;
        final long size;

        Entry(String id, String path, long size) {
            this.id = id;
            this.path = path;
            this.size = size;
        }
    }

    private static final class Child {
        String id;
        String name;
        boolean dir;
        long size;
    }

    private static List<Child> children(ContentResolver resolver, Uri tree, String docId) {
        List<Child> out = new ArrayList<>();
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        String[] columns = {
            Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME,
            Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE,
        };
        try (Cursor c = resolver.query(uri, columns, null, null, null)) {
            while (c != null && c.moveToNext()) {
                Child ch = new Child();
                ch.id = c.getString(0);
                ch.name = c.getString(1);
                ch.dir = Document.MIME_TYPE_DIR.equals(c.getString(2));
                ch.size = c.isNull(3) ? 0 : c.getLong(3);
                out.add(ch);
            }
        }
        return out;
    }

    /**
     * The picked folder if it holds boot.HIP and font.HIP, or the first folder
     * directly inside it that does -- picking the folder above the game's is
     * the likeliest mistake.
     */
    private static String findGameRoot(ContentResolver resolver, Uri tree, String docId) {
        List<Child> top = children(resolver, tree, docId);
        if (hasGame(top)) {
            return docId;
        }
        for (Child ch : top) {
            if (ch.dir && hasGame(children(resolver, tree, ch.id))) {
                return ch.id;
            }
        }
        return null;
    }

    private static boolean hasGame(List<Child> list) {
        boolean boot = false;
        boolean font = false;
        for (Child ch : list) {
            boot |= !ch.dir && ch.name.equalsIgnoreCase("boot.hip");
            font |= !ch.dir && ch.name.equalsIgnoreCase("font.hip");
        }
        return boot && font;
    }

    /** The files the game reads, by the Xbox disc's layout. */
    private static void collect(ContentResolver resolver, Uri tree, String docId, String prefix,
            boolean top, List<Entry> out) {
        for (Child ch : children(resolver, tree, docId)) {
            String lower = ch.name.toLowerCase(Locale.ROOT);
            if (ch.dir) {
                // Level folders are two characters (hb, jf, b1); fmv holds the
                // movies. Nothing deeper is read.
                if (top && (lower.length() == 2 || lower.equals("fmv"))) {
                    collect(resolver, tree, ch.id, ch.name + "/", false, out);
                }
                continue;
            }
            boolean wanted = lower.endsWith(".hip") || lower.endsWith(".hop")
                    || (lower.endsWith(".xmv") && prefix.equalsIgnoreCase("fmv/"))
                    || (top && lower.equals("sb.ini"));
            if (wanted) {
                out.add(new Entry(ch.id, prefix + ch.name, ch.size));
            }
        }
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) {
            for (File k : kids) {
                deleteTree(k);
            }
        }
        if (f.exists() && !f.delete()) {
            Log.w(TAG, "could not delete " + f);
        }
    }

    private static long treeSize(File f) {
        File[] kids = f.listFiles();
        if (kids == null) {
            return f.isFile() ? f.length() : 0;
        }
        long total = 0;
        for (File k : kids) {
            total += treeSize(k);
        }
        return total;
    }

        private static String gigabytes(long bytes) {
        return String.format(Locale.ROOT, "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

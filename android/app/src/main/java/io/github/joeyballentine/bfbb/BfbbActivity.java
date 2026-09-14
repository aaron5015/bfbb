package io.github.joeyballentine.bfbb;

import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

import org.libsdl.app.SDLActivity;

/**
 * The Activity the framework starts, and the whole of the port's Java.
 *
 * <p>Everything the game does is native; SDLActivity is what stands between it
 * and the Android framework, and this subclass exists to change exactly two
 * things about that.
 */
public class BfbbActivity extends SDLActivity {

    private static final String TAG = "bfbb";

    /**
     * The native libraries to load, and there is only one.
     *
     * <p>SDLActivity's default is {@code {"SDL3", "main"}}, which is right for
     * a build with a shared libSDL3.so beside the game. This build links SDL
     * statically into libmain.so (CMakeLists.txt forces the vendored, static
     * SDL on Android), so there is no second library and asking for one is an
     * UnsatisfiedLinkError before any of the port runs.
     *
     * <p>SDL's own JNI_OnLoad is inside libmain.so as a result, which is what
     * registers the native methods SDLActivity calls. It survives static
     * linking because SDL's CMake adds {@code -Wl,-u,JNI_OnLoad} to the static
     * target; nothing in this project refers to it, so without that the
     * linker would have no reason to keep the object that defines it.
     */
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    /**
     * Makes the assets directory before the game looks for it.
     *
     * <p>The game's data is the one thing the port cannot ship and cannot
     * guess: thirty-odd gigabytes extracted from a disc the player owns. It
     * goes in this application's own external files directory, which is the
     * only place on a modern Android that both sides can reach without a
     * permission dialog or a document picker; the native side finds it there
     * (see src/SB/Core/pc/android/iAndroid.cpp) and falls back to config.ini
     * if it is missing.
     *
     * <p>Creating it here rather than leaving it to the player is the
     * difference between "copy your files into Android/data/&lt;package&gt;
     * /files/assets" being an instruction someone can follow and one that
     * requires making three directories over USB first. The directory being
     * empty is not a problem this can solve; the game says so on its own.
     */
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        File external = getExternalFilesDir(null);

        if (external != null) {
            File assets = new File(external, "assets");

            if (!assets.exists() && !assets.mkdirs()) {
                // Not fatal. The native side treats a missing directory the
                // same as an empty one and reports it in the log with the
                // path, which is more use than a toast nobody screenshots.
                Log.w(TAG, "could not create " + assets.getAbsolutePath());
            }
        } else {
            Log.w(TAG, "no external files directory; assets must come from config.ini");
        }

        extractButtonGlyphs();

        super.onCreate(savedInstanceState);

        // Before SDL_main, which starts once the surface exists. See
        // ProfileActivity, and ResolveVideoProfile in iSystem.cpp.
        String profile = getSharedPreferences(ProfileActivity.PREFS, MODE_PRIVATE)
                .getString(ProfileActivity.KEY_PROFILE, ProfileActivity.MODERN);
        try {
            nativeSetenv("BFBB_PROFILE", profile);

            // A folder used in place, ahead of iAndroidStartup's default of the
            // app's own copy. See ImportActivity.
            String linked = ImportActivity.linkedPath(this);
            if (linked != null) {
                nativeSetenv("BFBB_ASSETS", linked);
            }
        } catch (UnsatisfiedLinkError e) {
            // libmain.so did not load; SDLActivity has already said so.
            Log.w(TAG, "could not pass the video profile", e);
        }
    }

    /**
     * Copies the button prompt glyphs out of the APK into internal storage,
     * where the game reads them from (files/buttons/&lt;set&gt;/*.png).
     *
     * <p>Once per install or update: a marker holds the package's last update
     * time, so a new APK replaces the old glyphs and an unchanged one costs a
     * single small read.
     */
    private void extractButtonGlyphs() {
        String stamp;
        try {
            stamp = Long.toString(getPackageManager()
                    .getPackageInfo(getPackageName(), 0).lastUpdateTime);
        } catch (PackageManager.NameNotFoundException e) {
            return;
        }

        File root = new File(getFilesDir(), "buttons");
        File marker = new File(root, ".extracted");

        if (stamp.equals(readSmallFile(marker))) {
            return;
        }

        try {
            copyAssetTree(getAssets(), "buttons", root);
            writeSmallFile(marker, stamp);
        } catch (IOException e) {
            // Not fatal: the game draws the disc's own prompts without them.
            Log.w(TAG, "could not extract the button glyphs", e);
        }
    }

    private static void copyAssetTree(AssetManager assets, String path, File dest)
            throws IOException {
        String[] children = assets.list(path);

        if (children == null || children.length == 0) {
            // A file. AssetManager.list gives nothing for one.
            File parent = dest.getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs()) {
                throw new IOException("could not create " + parent);
            }
            try (InputStream in = assets.open(path);
                 OutputStream out = new FileOutputStream(dest)) {
                byte[] buffer = new byte[16384];
                int n;
                while ((n = in.read(buffer)) > 0) {
                    out.write(buffer, 0, n);
                }
            }
            return;
        }

        if (!dest.exists() && !dest.mkdirs()) {
            throw new IOException("could not create " + dest);
        }
        for (String child : children) {
            copyAssetTree(assets, path + "/" + child, new File(dest, child));
        }
    }

    private static String readSmallFile(File file) {
        try (InputStream in = new FileInputStream(file)) {
            byte[] buffer = new byte[64];
            int n = in.read(buffer);
            return n > 0 ? new String(buffer, 0, n, StandardCharsets.UTF_8) : null;
        } catch (IOException e) {
            return null;
        }
    }

    private static void writeSmallFile(File file, String text) throws IOException {
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }
}

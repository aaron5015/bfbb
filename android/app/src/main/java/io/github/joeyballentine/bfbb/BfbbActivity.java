package io.github.joeyballentine.bfbb;

import android.os.Bundle;
import android.util.Log;

import java.io.File;

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

        super.onCreate(savedInstanceState);
    }
}

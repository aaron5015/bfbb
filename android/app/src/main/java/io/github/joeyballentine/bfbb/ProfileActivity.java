package io.github.joeyballentine.bfbb;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;

/**
 * The launcher entry: picks the video profile, then starts the game.
 *
 * <p>The first launch asks "vanilla" or "modern". The answer is kept, and the
 * app icon's long-press shortcuts change it. BfbbActivity hands it to the game
 * as BFBB_PROFILE, which src/SB/Core/pc/iSystem.cpp reads over config.ini.
 */
public class ProfileActivity extends Activity {

    static final String PREFS = "bfbb";
    static final String KEY_PROFILE = "profile";
    static final String EXTRA_PROFILE = "profile";

    static final String VANILLA = "vanilla";
    static final String MODERN = "modern";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);

        String fromShortcut = getIntent().getStringExtra(EXTRA_PROFILE);
        if (VANILLA.equals(fromShortcut) || MODERN.equals(fromShortcut)) {
            prefs.edit().putString(KEY_PROFILE, fromShortcut).apply();
        }

        if (prefs.contains(KEY_PROFILE)) {
            startGame();
            return;
        }

        String[] choices = {
            getString(R.string.profile_modern_choice),
            getString(R.string.profile_vanilla_choice),
        };

        new AlertDialog.Builder(this)
                .setTitle(R.string.profile_title)
                .setItems(choices, (dialog, which) -> {
                    prefs.edit().putString(KEY_PROFILE, which == 0 ? MODERN : VANILLA).apply();
                    startGame();
                })
                .setOnCancelListener(dialog -> finish())
                .show();
    }

    private void startGame() {
        startActivity(new Intent(this, BfbbActivity.class));
        finish();
        overridePendingTransition(0, 0);
    }
}

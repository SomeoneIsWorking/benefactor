package io.github.someoneisworking.benefactor;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Bundle;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

import io.github.someoneisworking.android.AndroidActivity;
import io.github.someoneisworking.android.AndroidDocumentImport;

/**
 * Title-owned activity. The setup screen itself is drawn in-app by the shared
 * setup-ui host; this class only supplies Android's own document picker and
 * hands the chosen disk images back to native code. No browser and no system
 * message box are involved in setup.
 */
public final class BenefactorActivity extends AndroidActivity {
    private static final int REQUEST_DISK_DIRECTORY = 4101;

    /* Launch options SDL passes to the native entry point as command-line
     * arguments, mirroring the desktop launcher: an Android process has no
     * command line and no environment of its own, so an explicit intent extra is
     * the only way to reach them. Absent extras add no argument, so an ordinary
     * launch behaves exactly as before. */
    private static final String EXTRA_LEVEL = "benefactor.level";
    private static final String EXTRA_LOAD = "benefactor.load";
    /* The diagnostic control channel binds to localhost and only when asked for
     * here, which is how a host drives a device run without a keyboard. */
    private static final String EXTRA_HTTP = "benefactor.http";
    private static final AndroidDocumentImport.Limits IMPORT_LIMITS =
            new AndroidDocumentImport.Limits(128, 16L * 1024L * 1024L, 64 * 1024);

    private AndroidDocumentImport importer;
    private AndroidDocumentImport.Result staged;
    private boolean importPending;

    private static native void nativeDiskSelectionResult(String stagingDirectory,
            String[] documentNames, String error);

    private static native void nativeDiskSelectionProgress(double fraction);

    /* Latest release tag, or a reason the check could not run. */
    private static native void nativeUpdateResult(String tag, String error);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        importer = new AndroidDocumentImport(this, IMPORT_LIMITS);
        importer.cleanStaleImports();
    }

    @Override
    protected String[] getArguments() {
        List<String> arguments = new ArrayList<>();
        Intent intent = getIntent();
        int level = intent.getIntExtra(EXTRA_LEVEL, 0);
        if (level > 0) {
            arguments.add("--level");
            arguments.add(Integer.toString(level));
        }
        String load = intent.getStringExtra(EXTRA_LOAD);
        if (load != null && !load.isEmpty()) {
            arguments.add("--load");
            arguments.add(load);
        }
        int http = intent.getIntExtra(EXTRA_HTTP, 0);
        if (http > 0) {
            arguments.add("--http");
            arguments.add(Integer.toString(http));
        }
        return arguments.toArray(new String[0]);
    }

    @Override
    protected void onDestroy() {
        if (importer != null) {
            importer.cancel();
        }
        if (importPending) {
            importPending = false;
            nativeDiskSelectionResult(null, new String[0],
                    "The activity closed during disk selection.");
        }
        super.onDestroy();
    }

    /**
     * Asks the release service for the latest version. Called from native code
     * once per run when the update check is enabled, and given the address so
     * that this side never holds a second copy of which service to ask. The
     * request runs on its own thread because the native side must never block on
     * the network, and the result is handed back through nativeUpdateResult.
     */
    public void checkForBenefactorUpdates(String url) {
        Thread request = new Thread(() -> {
            String tag = null;
            String error = null;
            HttpURLConnection connection = null;
            try {
                connection = (HttpURLConnection) new URL(url).openConnection();
                connection.setRequestMethod("GET");
                connection.setConnectTimeout(10000);
                connection.setReadTimeout(10000);
                connection.setRequestProperty("Accept", "application/vnd.github+json");
                connection.setRequestProperty("User-Agent", "benefactor-update-check");
                int status = connection.getResponseCode();
                if (status != 200) {
                    error = "the release service refused the request";
                } else {
                    String body = readAll(connection.getInputStream());
                    tag = releaseTag(body);
                    if (tag == null) {
                        error = "no release tag in the response";
                    }
                }
            } catch (IOException | SecurityException failure) {
                // A refused permission must not take the game down with it: the
                // check reports that it could not run, which is what it is.
                error = "the update check could not reach the network";
            } finally {
                if (connection != null) {
                    connection.disconnect();
                }
            }
            nativeUpdateResult(tag, error);
        }, "benefactor-update-check");
        request.setDaemon(true);
        request.start();
    }

    /* A release document is a few kilobytes. The bound is here so a server that
       streams without end cannot be read into memory. */
    private static final int MAX_RELEASE_BYTES = 65536;

    private static String readAll(InputStream stream) throws IOException {
        StringBuilder body = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(stream, StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null && body.length() < MAX_RELEASE_BYTES) {
                body.append(line).append('\n');
            }
        }
        return body.toString();
    }

    /* "tag_name" from a GitHub release document. Kept here, next to the request,
     * as the browser-free half of the check; the comparison is native. */
    private static String releaseTag(String json) {
        int at = json.indexOf("\"tag_name\"");
        if (at < 0) {
            return null;
        }
        int open = json.indexOf('"', json.indexOf(':', at) + 1);
        if (open < 0) {
            return null;
        }
        int close = json.indexOf('"', open + 1);
        if (close < 0) {
            return null;
        }
        return json.substring(open + 1, close);
    }

    /**
     * Opens Android's own document picker so the player can select either the
     * three original disk images or one ZIP containing them. Called from native
     * code when the setup screen's choose control is pressed; the staged files
     * are handed back and validated in-app.
     */
    public void pickBenefactorDisks() {
        runOnUiThread(() -> {
            if (importPending) {
                return;
            }
            importPending = true;
            releaseStagedImport();
            importer.setProgressListener((entries, bytes, totalBytes, name) -> {
                if (totalBytes > 0) {
                    nativeDiskSelectionProgress(Math.min(1.0, (double) bytes / (double) totalBytes));
                }
            });
            importer.pickDocuments(REQUEST_DISK_DIRECTORY, new AndroidDocumentImport.Callback() {
                @Override
                public void onImported(AndroidDocumentImport.Result result) {
                    importPending = false;
                    staged = result;
                    nativeDiskSelectionProgress(1.0);
                    // Report the facts: the private directory Android staged
                    // into and the display names it staged there. Which of them
                    // form a disk set is the title's decision, made natively.
                    nativeDiskSelectionResult(result.stagingDirectory.getAbsolutePath(),
                            result.documentNames.toArray(new String[0]), null);
                }

                @Override
                public void onCancelled() {
                    importPending = false;
                    nativeDiskSelectionResult(null, new String[0], null);
                }

                @Override
                public void onFailed(String message) {
                    importPending = false;
                    nativeDiskSelectionResult(null, new String[0], message);
                }
            });
        });
    }

    /**
     * Called from native code once it holds its own copy of the selection: the
     * staging directory Android's picker filled is then disposable, and keeping
     * it would retain every selected file until the app is uninstalled.
     */
    public void releaseBenefactorStaging() {
        runOnUiThread(this::releaseStagedImport);
    }

    private void releaseStagedImport() {
        final AndroidDocumentImport.Result result = staged;
        staged = null;
        if (result == null || importer == null) {
            return;
        }
        try {
            importer.discard(result);
        } catch (java.io.IOException error) {
            // The staged copy is a convenience; a failure to remove it must not
            // change the setup result the player already sees.
            android.util.Log.w("Benefactor", "could not release staged import: " + error.getMessage());
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, android.content.Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (importer != null) {
            importer.handleActivityResult(requestCode, resultCode, data);
        }
    }

    /** SDL rewrites orientation after its native window exists; restore the title contract there. */
    public void enforceBenefactorWindowPolicy() {
        runOnUiThread(() -> {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
            hideSystemUI();
        });
    }

}

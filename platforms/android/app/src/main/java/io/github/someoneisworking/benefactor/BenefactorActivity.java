package io.github.someoneisworking.benefactor;

import android.content.pm.ActivityInfo;
import android.os.Bundle;

import java.io.File;

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
    private static final AndroidDocumentImport.Limits IMPORT_LIMITS =
            new AndroidDocumentImport.Limits(128, 16L * 1024L * 1024L, 64 * 1024);

    private AndroidDocumentImport importer;
    private AndroidDocumentImport.Result staged;
    private boolean importPending;

    private static native void nativeDiskSelectionResult(String stagingDirectory,
            String[] documentNames, String error);

    private static native void nativeDiskSelectionProgress(double fraction);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        importer = new AndroidDocumentImport(this, IMPORT_LIMITS);
        importer.cleanStaleImports();
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

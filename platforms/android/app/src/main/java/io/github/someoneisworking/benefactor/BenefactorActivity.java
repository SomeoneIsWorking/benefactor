package io.github.someoneisworking.benefactor;

import android.app.AlertDialog;
import android.os.Bundle;

import io.github.someoneisworking.lucent.LucentActivity;
import io.github.someoneisworking.lucent.LucentDocumentImport;

/** Title-owned setup policy over Lucent's bounded, persisted SAF importer. */
public final class BenefactorActivity extends LucentActivity {
    private static final int REQUEST_DISK_DIRECTORY = 4101;
    private static final LucentDocumentImport.Limits IMPORT_LIMITS =
            new LucentDocumentImport.Limits(128, 16L * 1024L * 1024L, 64 * 1024);
    private LucentDocumentImport importer;
    private LucentDocumentImport.Result pendingImport;

    private static native void nativeDiskDirectoryResult(String directory, String error);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        importer = new LucentDocumentImport(this, IMPORT_LIMITS);
        importer.cleanStaleImports();
    }

    @Override
    protected void onDestroy() {
        if (importer != null) importer.cancel();
        nativeDiskDirectoryResult(null, "The Android activity closed before disk setup completed.");
        super.onDestroy();
    }

    public void requestBenefactorDisks(String reason) {
        runOnUiThread(() -> new AlertDialog.Builder(this)
                .setTitle("Benefactor disks required")
                .setMessage(reason)
                .setNegativeButton("Cancel", (dialog, which) -> nativeDiskDirectoryResult(null, null))
                .setPositiveButton("Browse", (dialog, which) -> importer.pickTree(REQUEST_DISK_DIRECTORY,
                        new LucentDocumentImport.Callback() {
                            @Override public void onImported(LucentDocumentImport.Result result) {
                                try {
                                    if (!hasDiskSet(result.stagingDirectory)) {
                                        nativeDiskDirectoryResult(null,
                                                "The folder must contain readable Disk.1, Disk.2, and Disk.3 files.");
                                        return;
                                    }
                                    pendingImport = result;
                                    nativeDiskDirectoryResult(result.stagingDirectory.getAbsolutePath(), null);
                                } catch (RuntimeException error) {
                                    nativeDiskDirectoryResult(null, "Android could not validate the disk set.");
                                }
                            }
                            @Override public void onCancelled() { nativeDiskDirectoryResult(null, null); }
                            @Override public void onFailed(String message) { nativeDiskDirectoryResult(null, message); }
                        }))
                .setCancelable(false)
                .show());
    }

    private static boolean hasDiskSet(java.io.File directory) {
        for (int index = 1; index <= 3; ++index) {
            java.io.File disk = new java.io.File(directory, "Disk." + index);
            if (!disk.isFile() || !disk.canRead()) return false;
        }
        return true;
    }

    /** Called after native validation, so a failed import never displaces the current installation. */
    public String commitBenefactorDisks(String stagingPath) {
        if (pendingImport == null || !pendingImport.stagingDirectory.getAbsolutePath().equals(stagingPath)
                || !hasDiskSet(pendingImport.stagingDirectory)) return null;
        try {
            java.io.File installed = importer.promoteValidated(pendingImport, "benefactor-disks");
            pendingImport = null;
            return installed.getAbsolutePath();
        } catch (java.io.IOException error) {
            return null;
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, android.content.Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (importer != null) importer.handleActivityResult(requestCode, resultCode, data);
    }
}

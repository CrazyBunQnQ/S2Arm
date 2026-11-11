// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
package org.s2arm.android;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/**
 * The player's own touch layout, one F01 file per game (iOS keeps one "mine" layout per game too).
 * No Android types: the directory comes from the Activity, so host tests can use a temp folder.
 * A missing file means the built-in layout; a damaged one is reported, never half-applied.
 */
final class TouchLayoutStore {
    static final String MINE = "mine";
    private final File file;

    /** title: the game's 16-digit title id; anything else (diagnostic app) shares one "app" file. */
    TouchLayoutStore(File directory, String title) {
        String key = title != null && title.matches("[0-9A-Fa-f]{16}") ? title.toUpperCase(java.util.Locale.ROOT) : "app";
        file = new File(directory, key + ".json");
    }

    File file() { return file; }

    /** The saved layout, or null when none is saved. IllegalArgumentException: the file is not valid F01. */
    TouchLayout load() throws IOException {
        if (!file.isFile()) return null;
        if (file.length() > 65536) throw new IllegalArgumentException("F01: layout must be at most 64 KiB");
        return TouchLayout.read(Files.readAllBytes(file.toPath()));
    }

    /** Write-then-rename, so a crash leaves the old layout or the new one, never a torn file. */
    void save(TouchLayout layout) throws IOException {
        byte[] data = layout.jsonBytes();
        TouchLayout.read(data);   // never write what load() would reject
        File directory = file.getParentFile();
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("cannot create " + directory);
        File temporary = new File(directory, file.getName() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(temporary)) {
            out.write(data);
            out.getFD().sync();
        }
        Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
    }

    /** Back to the built-in layout. */
    void reset() throws IOException {
        Files.deleteIfExists(file.toPath());
    }
}

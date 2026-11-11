// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
package org.s2arm.android;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * F01 schemaVersion 1, shared with ios/layouts/layout.schema.json.
 * Pure data model: no file I/O, Android API, input state or implicit selection.
 * Edits return a fully validated independent model, so failed edits preserve the old one.
 */
final class TouchLayout {
    private static final int MAX_BYTES = 65536;
    private static final double TOLERANCE = 1e-12;
    private static final Set<String> REQUIRED = new HashSet<>(Arrays.asList(
            "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"));

    static final class Rect {
        final double x, y, width, height;
        private Rect(double x, double y, double width, double height) {
            this.x = x; this.y = y; this.width = width; this.height = height;
        }
    }

    static final class Button {
        final String id;
        final double x, y, width, height, cornerRadius;
        private Button(String id, double x, double y, double width, double height, double cornerRadius) {
            this.id = id; this.x = x; this.y = y;
            this.width = width; this.height = height; this.cornerRadius = cornerRadius;
            require(REQUIRED.contains(id) || "LS".equals(id) || "RS".equals(id), "unknown button id");
            range(x, 0, 1); range(y, 0, 1);
            range(width, 0.04, 0.4); range(height, 0.04, 0.4); range(cornerRadius, 0, 0.2);
            require(cornerRadius <= Math.min(width, height) / 2 + TOLERANCE, "corner radius exceeds half side");
        }
    }

    static final class Stick {
        final String id;
        final Rect activation;
        final double radius;
        private Stick(String id, double x, double y, double width, double height, double radius) {
            this.id = id; this.radius = radius;
            activation = new Rect(x, y, width, height);
            require("left".equals(id) || "right".equals(id), "unknown stick id");
            range(x, 0, 1); range(y, 0, 1); range(width, 0.01, 1); range(height, 0.01, 1);
            require(x + width <= 1 + TOLERANCE && y + height <= 1 + TOLERANCE, "activation out of bounds");
            range(radius, 0.04, 0.3);
        }
    }

    final String identifier, name;
    final List<Button> buttons;
    final List<Stick> sticks;

    private TouchLayout(String identifier, String name, List<Button> buttons, List<Stick> sticks) {
        require(identifier.matches("[a-z0-9][a-z0-9._-]{0,63}"), "invalid identity");
        require(!name.isEmpty() && name.length() <= 64, "name must have 1..64 UTF-16 units");
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            // Match Foundation controlCharacterSet (Cc + Cf), not just ASCII/C1 controls.
            require(!Character.isISOControl(c) && Character.getType(name.codePointAt(i)) != Character.FORMAT,
                    "control or format character in name");
            if (Character.isHighSurrogate(c)) {
                require(i + 1 < name.length() && Character.isLowSurrogate(name.charAt(i + 1)), "invalid Unicode name");
                i++;
            } else require(!Character.isLowSurrogate(c), "invalid Unicode name");
        }
        require(buttons.size() >= 14 && buttons.size() <= 16, "need 14..16 buttons");
        Set<String> seen = new HashSet<>();
        for (Button button : buttons) require(seen.add(button.id), "duplicate button id");
        require(seen.containsAll(REQUIRED), "missing required button");
        require(sticks.size() == 2, "need two sticks");
        require(!sticks.get(0).id.equals(sticks.get(1).id), "duplicate stick id");
        Rect a = sticks.get(0).activation, b = sticks.get(1).activation;
        double overlapWidth = Math.min(a.x + a.width, b.x + b.width) - Math.max(a.x, b.x);
        double overlapHeight = Math.min(a.y + a.height, b.y + b.height) - Math.max(a.y, b.y);
        if (overlapWidth > TOLERANCE && overlapHeight > TOLERANCE)
            throw new IllegalArgumentException("F01: stick activation regions overlap");
        this.identifier = identifier; this.name = name;
        this.buttons = Collections.unmodifiableList(new ArrayList<>(buttons));
        this.sticks = Collections.unmodifiableList(new ArrayList<>(sticks));
    }

    /**
     * Android's built-in pad as F01, recomputed for each pad box like iOS defaultForSafeRect.
     * Positions are the historical TouchPad table (fractions of the box; offsets in u = min(w, h) / 10),
     * sizes are diameters in short-side units. The D-pad is four 0.08 keys 0.08 from its centre, so the
     * derived D-pad disc (reach + half key, see dpad()) keeps the old 0.12 radius. A stick's activation
     * is a radius-sized square on the fixed stick centre (Android sticks do not float).
     */
    static TouchLayout androidDefault(double width, double height) {
        viewport(0, 0, width, height);
        double unit = Math.min(width, height), ux = unit / 10 / width, uy = unit / 10 / height;
        List<Button> buttons = new ArrayList<>();
        double px = 0.27, py = 0.82, pd = 0.08, ps = 0.08;
        buttons.add(new Button("UP", px, py - pd * unit / height, ps, ps, ps / 2));
        buttons.add(new Button("DOWN", px, py + pd * unit / height, ps, ps, ps / 2));
        buttons.add(new Button("LEFT", px - pd * unit / width, py, ps, ps, ps / 2));
        buttons.add(new Button("RIGHT", px + pd * unit / width, py, ps, ps, ps / 2));
        Object[][] keys = {
            {"A", 0.93, 0.56, 0.55}, {"B", 0.87, 0.56 + 1.15 * uy, 0.55},
            {"X", 0.87, 0.56 - 1.15 * uy, 0.55}, {"Y", 0.81, 0.56, 0.55},
            {"LS", 0.13 + 1.9 * ux, 0.56 - 1.3 * uy, 0.38}, {"RS", 0.73 - 1.7 * ux, 0.80 - 1.1 * uy, 0.38},
            {"L", 0.17, 0.15, 0.50}, {"R", 0.83, 0.15, 0.50}, {"ZL", 0.07, 0.15, 0.50}, {"ZR", 0.93, 0.15, 0.50},
            {"PLUS", 0.58, 0.12, 0.40}, {"MINUS", 0.42, 0.12, 0.40}};
        for (Object[] k : keys) {
            double side = (double) k[3] / 5;   // diameter 2r * u, in units of 10u
            buttons.add(new Button((String) k[0], (double) k[1], (double) k[2], side, side, side / 2));
        }
        List<Stick> sticks = new ArrayList<>();
        sticks.add(fixedStick("left", 0.13, 0.56, 0.145, unit, width, height));
        sticks.add(fixedStick("right", 0.73, 0.80, 0.12, unit, width, height));
        return new TouchLayout("default", "default", buttons, sticks);
    }

    private static Stick fixedStick(String id, double cx, double cy, double radius,
                                    double unit, double width, double height) {
        double w = radius * unit / width, h = radius * unit / height;
        return new Stick(id, cx - w / 2, cy - h / 2, w, h, radius);
    }

    /** Centre of the stick's activation region, in viewport pixels (where Android's fixed stick sits). */
    double[] stickCentre(String id, double left, double top, double width, double height) {
        viewport(left, top, width, height);
        Rect a = stick(id).activation;
        return new double[] {left + (a.x + a.width / 2) * width, top + (a.y + a.height / 2) * height};
    }

    /**
     * One disc over the four direction keys (iOS dpadDirectionsAt): centre = mean of their centres,
     * radius = farthest centre + half the widest key side. Returns {x, y, radius} in viewport pixels.
     */
    double[] dpad(double left, double top, double width, double height) {
        String[] ids = {"UP", "DOWN", "LEFT", "RIGHT"};
        double sx = 0, sy = 0, reach = 0, half = 0;
        Rect[] frames = new Rect[4];
        for (int i = 0; i < 4; i++) {
            frames[i] = frameForButton(ids[i], left, top, width, height);
            sx += frames[i].x + frames[i].width / 2;
            sy += frames[i].y + frames[i].height / 2;
        }
        double cx = sx / 4, cy = sy / 4;
        for (Rect f : frames) {
            reach = Math.max(reach, Math.hypot(f.x + f.width / 2 - cx, f.y + f.height / 2 - cy));
            half = Math.max(half, Math.max(f.width, f.height) / 2);
        }
        return new double[] {cx, cy, reach + half};
    }

    static TouchLayout read(byte[] data) {
        require(data != null && data.length <= MAX_BYTES, "layout must be at most 64 KiB");
        String text;
        try {
            text = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(data)).toString();
        } catch (CharacterCodingException error) {
            throw new IllegalArgumentException("F01: invalid UTF-8", error);
        }
        Map<String, Object> doc = fields(Json.parseStrict(text), "schemaVersion", "id", "name", "buttons", "sticks");
        range(number(doc, "schemaVersion"), 1, 1);
        List<Button> buttons = new ArrayList<>();
        for (Object item : Json.array(doc.get("buttons"), "buttons")) {
            Map<String, Object> b = fields(item, "id", "x", "y", "width", "height", "cornerRadius");
            buttons.add(new Button(string(b, "id"), number(b, "x"), number(b, "y"),
                    number(b, "width"), number(b, "height"), number(b, "cornerRadius")));
        }
        List<Stick> sticks = new ArrayList<>();
        for (Object item : Json.array(doc.get("sticks"), "sticks")) {
            Map<String, Object> s = fields(item, "id", "activation", "radius");
            Map<String, Object> a = fields(s.get("activation"), "x", "y", "width", "height");
            sticks.add(new Stick(string(s, "id"), number(a, "x"), number(a, "y"),
                    number(a, "width"), number(a, "height"), number(s, "radius")));
        }
        return new TouchLayout(string(doc, "id"), string(doc, "name"), buttons, sticks);
    }

    Button button(String id) {
        for (Button b : buttons) if (b.id.equals(id)) return b;
        throw new IllegalArgumentException("F01: unknown button " + id);
    }

    Stick stick(String id) {
        for (Stick s : sticks) if (s.id.equals(id)) return s;
        throw new IllegalArgumentException("F01: unknown stick " + id);
    }

    TouchLayout withButton(String id, double x, double y, double width, double height, double cornerRadius) {
        List<Button> edited = new ArrayList<>(buttons);
        edited.set(edited.indexOf(button(id)), new Button(id, x, y, width, height, cornerRadius));
        return new TouchLayout(identifier, name, edited, sticks);
    }

    TouchLayout withStick(String id, double x, double y, double width, double height, double radius) {
        List<Stick> edited = new ArrayList<>(sticks);
        edited.set(edited.indexOf(stick(id)), new Stick(id, x, y, width, height, radius));
        return new TouchLayout(identifier, name, buttons, edited);
    }

    /** Exporting a built-in layout must use a fresh user identity. No file is overwritten here. */
    TouchLayout copyAs(String id, String displayName) {
        require(id != null && displayName != null, "identity and name are required");
        require(!"default".equals(id) && !"touch".equals(id), "reserved built-in identity");
        return new TouchLayout(id, displayName, buttons, sticks);
    }

    /** Button centres use width/height; sizes use the short side, as in iOS frameForButton. */
    Rect frameForButton(String id, double left, double top, double width, double height) {
        Button b = button(id);
        return project(b.x, b.y, b.width, b.height, left, top, width, height);
    }

    /** Editor preview at the activation centre; runtime floating-stick capture is separate. */
    Rect frameForStick(String id, double left, double top, double width, double height) {
        Stick s = stick(id);
        Rect a = s.activation;
        double diameter = s.radius * 2;
        return project(a.x + a.width / 2, a.y + a.height / 2, diameter, diameter,
                left, top, width, height);
    }

    /**
     * Editor save (iOS layoutWithButtonCenters): dragged button centres in viewport pixels back to F01.
     * Centres are clamped to [0, 1]; sizes, corners, sticks and buttons without a new centre are kept.
     * Unlike iOS, an unknown id or a non-finite point is rejected instead of ignored.
     */
    TouchLayout withButtonCenters(Map<String, double[]> centres, double left, double top, double width, double height,
                                  String id, String displayName) {
        require(centres != null, "centres are required");
        viewport(left, top, width, height);
        for (String key : centres.keySet()) button(key);
        List<Button> edited = new ArrayList<>(buttons.size());
        for (Button b : buttons) {
            double[] p = centres.get(b.id);
            if (p == null) { edited.add(b); continue; }
            require(p.length == 2 && Double.isFinite(p[0]) && Double.isFinite(p[1]), "invalid centre");
            edited.add(new Button(b.id, clamp01((p[0] - left) / width), clamp01((p[1] - top) / height),
                    b.width, b.height, b.cornerRadius));
        }
        return new TouchLayout(identifier, name, edited, sticks).copyAs(id, displayName);
    }

    /** Editor hit test (iOS buttonAt:safeRect:slop:): first button whose frame grown by slop contains the point. */
    String buttonAt(double px, double py, double left, double top, double width, double height, double slop) {
        require(Double.isFinite(px) && Double.isFinite(py) && Double.isFinite(slop) && slop >= 0, "invalid hit point");
        for (Button b : buttons) {
            Rect f = frameForButton(b.id, left, top, width, height);
            if (px >= f.x - slop && px < f.x + f.width + slop && py >= f.y - slop && py < f.y + f.height + slop)
                return b.id;
        }
        return null;
    }

    private static double clamp01(double value) { return Math.max(0, Math.min(1, value)); }

    private static void viewport(double left, double top, double width, double height) {
        require(Double.isFinite(left) && Double.isFinite(top) && Double.isFinite(width)
                && Double.isFinite(height) && width > 0 && height > 0
                && Double.isFinite(left + width) && Double.isFinite(top + height), "invalid viewport");
    }

    private static Rect project(double x, double y, double normalizedWidth, double normalizedHeight,
                                double left, double top, double width, double height) {
        viewport(left, top, width, height);
        double unit = Math.min(width, height), w = normalizedWidth * unit, h = normalizedHeight * unit;
        return new Rect(Math.max(left, Math.min(left + x * width - w / 2, left + width - w)),
                Math.max(top, Math.min(top + y * height - h / 2, top + height - h)), w, h);
    }

    byte[] jsonBytes() {
        StringBuilder json = new StringBuilder("{\"schemaVersion\":1,\"id\":");
        quote(json, identifier); json.append(",\"name\":"); quote(json, name);
        json.append(",\"buttons\":[");
        for (int i = 0; i < buttons.size(); i++) {
            Button b = buttons.get(i);
            if (i != 0) json.append(',');
            json.append("{\"id\":"); quote(json, b.id);
            geometry(json, b.x, b.y, b.width, b.height);
            json.append(",\"cornerRadius\":").append(b.cornerRadius).append('}');
        }
        json.append("],\"sticks\":[");
        for (int i = 0; i < sticks.size(); i++) {
            Stick s = sticks.get(i); Rect a = s.activation;
            if (i != 0) json.append(',');
            json.append("{\"id\":"); quote(json, s.id);
            json.append(",\"activation\":{\"x\":").append(a.x).append(",\"y\":").append(a.y)
                    .append(",\"width\":").append(a.width).append(",\"height\":").append(a.height);
            json.append("},\"radius\":").append(s.radius).append('}');
        }
        return json.append("]}").toString().getBytes(StandardCharsets.UTF_8);
    }

    private static void geometry(StringBuilder json, double x, double y, double width, double height) {
        json.append(",\"x\":").append(x).append(",\"y\":").append(y)
                .append(",\"width\":").append(width).append(",\"height\":").append(height);
    }

    private static void quote(StringBuilder json, String value) {
        json.append('"');
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c == '"' || c == '\\') json.append('\\');
            json.append(c); // All strings were validated: no controls or unpaired surrogates.
        }
        json.append('"');
    }

    private static Map<String, Object> fields(Object value, String... names) {
        Map<String, Object> object = Json.object(value, "F01 fields");
        Set<String> expected = new HashSet<>(Arrays.asList(names));
        if (!object.keySet().equals(expected))
            throw new IllegalArgumentException("F01: missing or unknown fields");
        return object;
    }

    private static String string(Map<String, Object> object, String key) {
        Object value = object.get(key);
        require(value instanceof String, key + " must be a string");
        return (String) value;
    }

    private static double number(Map<String, Object> object, String key) {
        Object value = object.get(key);
        require(value instanceof Number, key + " must be a number");
        return ((Number) value).doubleValue();
    }

    private static void range(double value, double minimum, double maximum) {
        require(Double.isFinite(value) && value >= minimum && value <= maximum, "number out of range");
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new IllegalArgumentException("F01: " + message);
    }
}

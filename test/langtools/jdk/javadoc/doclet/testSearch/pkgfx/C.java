/*
 * Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package pkgfx;

public class C {

    /**
     * @propertySetter Property
     * @propertyDescription PropertyDescription
     */
    public void CC() {}

    /**
     * @propertyGetter Property
     *
     */
    public void B() {}

    /**
     * Method A documentation. Test index tag {@index "test treat as private"}.
     * @treatAsPrivate
     */
    public void A() {}

    /**
     * Field i
     * @defaultValue 1.0
     */
    public int i;


    /**
     * Defines the direction/speed at which the {@code Timeline} is expected to
     * be played.
     * @defaultValue 11
     * @since JavaFX 8.0
     */
    private DoubleProperty rate;

    public final void setRate(double value) {}

    public final double getRate() {return 0.0;}

    public final DoubleProperty rateProperty() {return null;}

    private BooleanProperty paused;

    public final void setPaused(boolean value) {}

    public final double isPaused() {return 0.0;}

    class DoubleProperty {}

    class BooleanProperty {}

    public final BooleanProperty setTestMethodProperty() {return null;}

    private class Inner {
        private BooleanProperty testMethodProperty() {return null;}

        /**
         * Defines the direction/speed at which the {@code Timeline} is expected to
         * be played.
         * @defaultValue 11
         */
        private DoubleProperty rate;

        public final void setRate(double value) {}

        public final double getRate() {return 0.0;}

        public final DoubleProperty rateProperty() {return null;}
    }
}

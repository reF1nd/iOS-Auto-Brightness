#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
: "${ANDROID_HOME:?Android SDK required}"
mkdir -p build/bridge/classes build/bridge/dex
"$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" 'platforms;android-35' 'build-tools;35.0.0'
mapfile -t sources < <(find bridge/src -name '*.java' -type f | sort)
javac -source 17 -target 17 -encoding UTF-8 -cp "$ANDROID_HOME/platforms/android-35/android.jar" -d build/bridge/classes "${sources[@]}"
jar --create --file build/bridge/classes.jar -C build/bridge/classes .
"$ANDROID_HOME/build-tools/35.0.0/d8" --min-api 28 --lib "$ANDROID_HOME/platforms/android-35/android.jar" --output build/bridge/dex build/bridge/classes.jar
jar --create --file build/bridge/bridge.jar -C build/bridge/dex classes.dex
javac -cp build/bridge/classes -d build/bridge/test-classes tests/EventGateTest.java tests/LightReadingTest.java tests/AdapterPolicyTest.java
java -cp build/bridge/classes:build/bridge/test-classes EventGateTest
java -cp build/bridge/classes:build/bridge/test-classes LightReadingTest
java -cp build/bridge/classes:build/bridge/test-classes AdapterPolicyTest

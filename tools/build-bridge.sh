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
javac -cp build/bridge/classes -d build/bridge/test-classes tests/EventGateTest.java tests/LightReadingTest.java tests/AdapterPolicyTest.java tests/LearningLuxTest.java
java -cp build/bridge/classes:build/bridge/test-classes EventGateTest
java -cp build/bridge/classes:build/bridge/test-classes LightReadingTest
java -cp build/bridge/classes:build/bridge/test-classes AdapterPolicyTest
java -cp build/bridge/classes:build/bridge/test-classes LearningLuxTest
javac -cp build/bridge/classes -d build/bridge/test-classes tests/CompatibilityTest.java
java -cp build/bridge/classes:build/bridge/test-classes CompatibilityTest
javac -cp build/bridge/classes -d build/bridge/test-classes tests/ProtectionPolicyTest.java
java -cp build/bridge/classes:build/bridge/test-classes ProtectionPolicyTest
javac -cp build/bridge/classes -d build/bridge/test-classes tests/ManualLearningTest.java
java -cp build/bridge/classes:build/bridge/test-classes ManualLearningTest
javac -cp build/bridge/classes -d build/bridge/test-classes tests/ParityCompletionTest.java
java -cp build/bridge/classes:build/bridge/test-classes ParityCompletionTest
javac -cp build/bridge/classes -d build/bridge/test-classes tests/InteractionGuardTest.java
java -cp build/bridge/classes:build/bridge/test-classes InteractionGuardTest

javac -cp build/bridge/classes -d build/bridge/test-classes tests/DirectBacklightTest.java
java -cp build/bridge/classes:build/bridge/test-classes DirectBacklightTest
javac -cp build/bridge/classes -d build/bridge/test-classes tests/DriverOutputGuardTest.java
java -cp build/bridge/classes:build/bridge/test-classes DriverOutputGuardTest
javac -cp build/bridge/classes -d build/bridge/test-classes tests/OutputProbeGuardTest.java
java -cp build/bridge/classes:build/bridge/test-classes OutputProbeGuardTest
javac -cp build/bridge/classes -d build/bridge/test-classes tests/ReferenceSliderSamplingTest.java
java -cp build/bridge/classes:build/bridge/test-classes ReferenceSliderSamplingTest
javac -cp build/bridge/classes -d build/bridge/test-classes tests/BacklightLeaseTest.java
java -cp build/bridge/classes:build/bridge/test-classes BacklightLeaseTest

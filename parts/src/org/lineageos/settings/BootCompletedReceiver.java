/*
 * Copyright (C) 2015 The CyanogenMod Project
 *               2017-2019 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.lineageos.settings;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.util.Log;

import org.lineageos.settings.display.DisplayNodes;
import org.lineageos.settings.utils.FileUtils;
import android.content.SharedPreferences;
import androidx.preference.PreferenceManager;

public class BootCompletedReceiver extends BroadcastReceiver {

    private static final boolean DEBUG = false;
    private static final String TAG = "XiaomiParts";


    @Override
    public void onReceive(final Context context, Intent intent) {
        if (DEBUG) Log.d(TAG, "Received boot completed intent");
        try {
            Class<?> clazz = Class.forName("org.lineageos.settings.refreshrate.RefreshUtils");
            java.lang.reflect.Method method = clazz.getMethod("initialize", Context.class);
            method.invoke(null, context);
        } catch (Exception e) {
            // Ignore
        }

        SharedPreferences sharedPrefs = PreferenceManager.getDefaultSharedPreferences(context);

        boolean dcDimmingEnabled = sharedPrefs.getBoolean(DisplayNodes.getDcDimmingEnableKey(), false);
        FileUtils.writeLine(DisplayNodes.getDcDimmingNode(), dcDimmingEnabled ? "1" : "0");
        boolean hbmEnabled = sharedPrefs.getBoolean(DisplayNodes.getHbmEnableKey(), false);
        FileUtils.writeLine(DisplayNodes.getHbmNode(), hbmEnabled ? "1" : "0");
    }
}

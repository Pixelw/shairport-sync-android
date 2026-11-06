// SPDX-License-Identifier: MIT
package org.shairport.sync.android;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** 使用系统开机及升级广播恢复已启用的常驻监听。 */
public final class BootReceiver extends BroadcastReceiver {
    /** 收到系统启动事件后及时启动前台服务。 */
    @Override public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();
        if ((Intent.ACTION_BOOT_COMPLETED.equals(action) || Intent.ACTION_MY_PACKAGE_REPLACED.equals(action))
                && context.getSharedPreferences("companion", 0).getBoolean("enabled", false)) {
            context.startForegroundService(new Intent(context, SPSCompanionService.class));
        }
    }
}

package com.fastenough.companion

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.Binder

/**
 * Tells a computer connected over ADB which companion device this is, so it can
 * pair the ADB connection and the companion connection of the same phone.
 * Only the ADB shell and root may read it.
 */
class CompanionInfoProvider : ContentProvider() {
    companion object {
        private const val SHELL_UID = 2000
        private const val ROOT_UID = 0
    }

    override fun onCreate(): Boolean = true

    override fun query(uri: Uri, projection: Array<out String>?, selection: String?,
                       selectionArgs: Array<out String>?, sortOrder: String?): Cursor? {
        val caller = Binder.getCallingUid()
        if (caller != SHELL_UID && caller != ROOT_UID) return null
        val context = context ?: return null
        CompanionSettings.init(context)
        return MatrixCursor(arrayOf("id")).apply { addRow(arrayOf(CompanionSettings.deviceId)) }
    }

    override fun getType(uri: Uri): String? = null
    override fun insert(uri: Uri, values: ContentValues?): Uri? = null
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int = 0
    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?): Int = 0
}

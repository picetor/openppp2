/******************************************************************************
 *                                                                            *
 * Copyright (C) 2021 by nekohasekai <contact-sagernet@sekai.icu>             *
 * Copyright (C) 2021 by Max Lv <max.c.lv@gmail.com>                          *
 * Copyright (C) 2021 by Mygod Studio <contact-shadowsocks-android@mygod.be>  *
 *                                                                            *
 * This program is free software: you can redistribute it and/or modify       *
 * it under the terms of the GNU General Public License as published by       *
 * the Free Software Foundation, either version 3 of the License, or          *
 *  (at your option) any later version.                                       *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program. If not, see <http://www.gnu.org/licenses/>.       *
 *                                                                            *
 ******************************************************************************/

package io.nekohasekai.sagernet.database.preference

import android.graphics.Typeface
import android.text.InputFilter
import android.text.InputType
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import androidx.preference.EditTextPreference


object EditTextPreferenceModifiers {
    object Monospace : EditTextPreference.OnBindEditTextListener {
        override fun onBindEditText(editText: EditText) {
            editText.typeface = Typeface.MONOSPACE
        }
    }

    object Hosts : EditTextPreference.OnBindEditTextListener {

        override fun onBindEditText(editText: EditText) {
            editText.setHorizontallyScrolling(true)
            editText.setSelection(editText.text.length)
        }
    }

    object Port : EditTextPreference.OnBindEditTextListener {
        private val portLengthFilter = arrayOf(InputFilter.LengthFilter(5))

        override fun onBindEditText(editText: EditText) {
            editText.inputType = EditorInfo.TYPE_CLASS_NUMBER
            editText.filters = portLengthFilter
            editText.setSingleLine()
            editText.setSelection(editText.text.length)
        }
    }

    object Number : EditTextPreference.OnBindEditTextListener {

        override fun onBindEditText(editText: EditText) {
            editText.inputType = EditorInfo.TYPE_CLASS_NUMBER
            editText.setSingleLine()
            editText.setSelection(editText.text.length)
        }
    }

    object Mux : EditTextPreference.OnBindEditTextListener {

        override fun onBindEditText(editText: EditText) {
            editText.inputType = EditorInfo.TYPE_CLASS_NUMBER or EditorInfo.TYPE_NUMBER_FLAG_SIGNED
            editText.setSingleLine()
            editText.setSelection(editText.text.length)
        }
    }

    /** Multi-line text editor: keeps newline-separated entries (one per line)
     *  visible in the dialog instead of collapsing them to a single row. */
    object Multiline : EditTextPreference.OnBindEditTextListener {

        override fun onBindEditText(editText: EditText) {
            editText.setSingleLine(false)
            editText.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE or
                InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
            editText.gravity = android.view.Gravity.TOP or android.view.Gravity.START
            editText.minLines = 6
            editText.maxLines = 12
            editText.setHorizontallyScrolling(false)
            editText.setSelection(editText.text.length)
        }
    }
}

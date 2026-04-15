package com.risegfx.android

import com.risegfx.android.nativebridge.DirtyRect
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Test

/**
 * Pure-JVM test for the dirty-rect packing/merging logic. Runs without an
 * emulator — fast feedback for regressions in [DirtyRect] serialization and
 * [RenderViewModel]'s merge strategy.
 */
class DirtyRectTest {

    @Test
    fun packAndUnpackRoundtrip() {
        val original = DirtyRect(top = 10, left = 20, bottom = 300, right = 400)
        val packed = (10L shl 48) or (20L shl 32) or (300L shl 16) or 400L
        val unpacked = DirtyRect.unpack(packed)
        assertEquals(original, unpacked)
    }

    @Test
    fun mergeExpandsBoundingBox() {
        val a = DirtyRect(top = 10, left = 10, bottom = 20, right = 20)
        val b = DirtyRect(top = 5,  left = 15, bottom = 25, right = 30)
        val m = DirtyRect.merge(a, b)
        assertNotNull(m)
        assertEquals(5,  m.top)
        assertEquals(10, m.left)
        assertEquals(25, m.bottom)
        assertEquals(30, m.right)
    }

    @Test
    fun mergeWithNullReturnsOther() {
        val r = DirtyRect(1, 2, 3, 4)
        assertEquals(r, DirtyRect.merge(null, r))
    }

    @Test
    fun widthHeightClampToZero() {
        val invalid = DirtyRect(top = 50, left = 50, bottom = 10, right = 10)
        assertEquals(0, invalid.width)
        assertEquals(0, invalid.height)
    }
}

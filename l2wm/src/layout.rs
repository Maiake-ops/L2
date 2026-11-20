#![no_std]
#[cfg(not(test))]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! { loop { core::hint::spin_loop(); } }

// C ABI: master takes the left portion, remaining clients stack on the right.
// When monocle is non-zero, every client takes the full screen.
#[no_mangle]
pub extern "C" fn l2wm_rect_ex(index: u32, count: u32, width: u32, height: u32,
                              master_pct: u32, monocle: u32, axis: u32) -> u32 {
    if count == 0 || index >= count { return 0; }
    if monocle != 0 {
        return match axis { 0 => 0, 1 => 0, 2 => width, _ => height };
    }
    let pct = if master_pct < 20 { 20 } else if master_pct > 80 { 80 } else { master_pct };
    let master = if count == 1 {
        width
    } else {
        let m = (width as u64 * pct as u64 / 100) as u32;
        if width >= 2 { m.clamp(1, width - 1) } else { m }
    };
    let (x, y, w, h) = if index == 0 { (0, 0, master, height) } else {
        let n = count - 1;
        let top = (height as u64 * (index - 1) as u64 / n as u64) as u32;
        let bottom = (height as u64 * index as u64 / n as u64) as u32;
        (master, top, width - master, bottom - top)
    };
    match axis { 0 => x, 1 => y, 2 => w, _ => h }
}

#[no_mangle]
pub extern "C" fn l2wm_rect(index: u32, count: u32, width: u32, height: u32,
                           axis: u32) -> u32 {
    l2wm_rect_ex(index, count, width, height, 50, 0, axis)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn layout_covers_odd_screen() {
        assert_eq!(l2wm_rect(0, 1, 801, 601, 2), 801);
        assert_eq!(l2wm_rect(1, 3, 801, 601, 2), 401);
        assert_eq!(l2wm_rect(1, 3, 801, 601, 3), 300);
        assert_eq!(l2wm_rect(2, 3, 801, 601, 1), 300);
        assert_eq!(l2wm_rect(2, 3, 801, 601, 3), 301);
        assert_eq!(l2wm_rect(0, 0, 801, 601, 2), 0);
    }

    #[test]
    fn master_ratio_and_clamping() {
        assert_eq!(l2wm_rect_ex(0, 2, 1000, 500, 60, 0, 2), 600);
        assert_eq!(l2wm_rect_ex(1, 2, 1000, 500, 60, 0, 0), 600);
        assert_eq!(l2wm_rect_ex(1, 2, 1000, 500, 60, 0, 2), 400);
        assert_eq!(l2wm_rect_ex(0, 2, 1000, 500, 0, 0, 2), 200);
        assert_eq!(l2wm_rect_ex(0, 2, 1000, 500, 100, 0, 2), 800);
    }

    #[test]
    fn monocle_layout_uses_full_screen() {
        for idx in 0..3 {
            assert_eq!(l2wm_rect_ex(idx, 3, 800, 600, 50, 1, 0), 0);
            assert_eq!(l2wm_rect_ex(idx, 3, 800, 600, 50, 1, 1), 0);
            assert_eq!(l2wm_rect_ex(idx, 3, 800, 600, 50, 1, 2), 800);
            assert_eq!(l2wm_rect_ex(idx, 3, 800, 600, 50, 1, 3), 600);
        }
        assert_eq!(l2wm_rect_ex(3, 3, 800, 600, 50, 1, 2), 0);
    }
}

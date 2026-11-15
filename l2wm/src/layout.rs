#![no_std]
#[cfg(not(test))]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! { loop { core::hint::spin_loop(); } }

// C ABI: master takes the left half, remaining clients stack on the right.
#[no_mangle]
pub extern "C" fn l2wm_rect(index: u32, count: u32, width: u32, height: u32,
                           axis: u32) -> u32 {
    if count == 0 || index >= count { return 0; }
    let master = if count == 1 { width } else { width / 2 };
    let (x, y, w, h) = if index == 0 { (0, 0, master, height) } else {
        let n = count - 1;
        let top = (height as u64 * (index - 1) as u64 / n as u64) as u32;
        let bottom = (height as u64 * index as u64 / n as u64) as u32;
        (master, top, width - master, bottom - top)
    };
    match axis { 0 => x, 1 => y, 2 => w, _ => h }
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
}

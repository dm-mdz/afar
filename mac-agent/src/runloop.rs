//! 主线程运行循环：私有接口的回调（虚拟屏、随航）派发在主队列上，必须让主 RunLoop 转起来才能收到。

use core_foundation_sys::runloop::{kCFRunLoopDefaultMode, kCFRunLoopRunFinished, CFRunLoopRunInMode};
use std::time::{Duration, Instant};

/// 在主线程上转 RunLoop 指定秒数。RunLoop 里没有事件源时会立刻返回，这里补睡眠，避免空转吃满 CPU。
pub fn run_for(seconds: f64) {
    let end = Instant::now() + Duration::from_secs_f64(seconds);
    loop {
        let left = end.saturating_duration_since(Instant::now());
        if left.is_zero() {
            break;
        }
        let r = unsafe { CFRunLoopRunInMode(kCFRunLoopDefaultMode, left.as_secs_f64(), 0) };
        if r == kCFRunLoopRunFinished {
            std::thread::sleep(left.min(Duration::from_millis(100)));
        }
    }
}

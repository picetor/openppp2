//! Traffic-rate sampling ring buffer (docs/RUST_TUI_DESIGN_CN.md §5.5.1).

/// Number of samples kept (1s poll ≈ 2 minutes of history).
pub const SAMPLE_CAPACITY: usize = 120;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RatePoint {
    pub rx_bytes_per_sec: u64,
    pub tx_bytes_per_sec: u64,
}

pub struct TrafficHistory {
    samples: Vec<RatePoint>,
    last_in: Option<u64>,
    last_out: Option<u64>,
    last_tick_ms: Option<u64>,
    total_rx: u64,
    total_tx: u64,
}

impl Default for TrafficHistory {
    fn default() -> Self {
        Self::new()
    }
}

impl TrafficHistory {
    pub fn new() -> Self {
        Self {
            samples: Vec::with_capacity(SAMPLE_CAPACITY),
            last_in: None,
            last_out: None,
            last_tick_ms: None,
            total_rx: 0,
            total_tx: 0,
        }
    }

    /// Feed a snapshot.
    ///
    /// NOTE: the core's `traffic.rx_bytes/tx_bytes` are the byte deltas of
    /// the last OnTick period (1 second window), NOT cumulative totals.
    /// They are therefore used directly as the current rates.  Totals are
    /// accumulated here for the "Total" display.  A core restart is
    /// detected by the monotonic tick going backwards or a >60s gap.
    /// Feed a snapshot.
    ///
    /// `in_bytes/out_bytes` are the core's CUMULATIVE traffic totals
    /// (statistics_snapshot.IncomingTraffic/OutgoingTraffic).  Differencing
    /// them against the previous sample yields the bytes transferred since
    /// the last snapshot; combined with the elapsed monotonic time this
    /// gives the rate.  Using the cumulative totals (instead of the core's
    /// per-period deltas) is robust: the core resets its delta baseline on
    /// EVERY GetTransmissionStatistics call (built-in TUI or RPC), so
    /// per-period deltas get corrupted by competing consumers.
    pub fn feed(&mut self, in_bytes: u64, out_bytes: u64, tick_ms: u64) {
        match (self.last_in, self.last_out, self.last_tick_ms) {
            (Some(prev_in), Some(prev_out), Some(prev_tick)) if tick_ms >= prev_tick => {
                let dt_ms = tick_ms - prev_tick;
                if dt_ms == 0 || dt_ms > 60_000 {
                    // Clock reset or a huge gap: treat as a fresh window.
                    self.reset();
                    self.last_in = Some(in_bytes);
                    self.last_out = Some(out_bytes);
                    self.last_tick_ms = Some(tick_ms);
                    return;
                }
                let scale = 1000.0 / dt_ms as f64;
                let point = RatePoint {
                    rx_bytes_per_sec: ((in_bytes.saturating_sub(prev_in)) as f64 * scale) as u64,
                    tx_bytes_per_sec: ((out_bytes.saturating_sub(prev_out)) as f64 * scale) as u64,
                };
                self.samples.push(point);
                if self.samples.len() > SAMPLE_CAPACITY {
                    self.samples.remove(0);
                }
            }
            _ => {
                // First sample: no rate yet.
            }
        }
        self.last_in = Some(in_bytes);
        self.last_out = Some(out_bytes);
        self.total_rx = in_bytes;
        self.total_tx = out_bytes;
        self.last_tick_ms = Some(tick_ms);
    }

    pub fn reset(&mut self) {
        self.samples.clear();
        self.last_in = None;
        self.last_out = None;
        self.last_tick_ms = None;
        self.total_rx = 0;
        self.total_tx = 0;
    }

    pub fn samples(&self) -> &[RatePoint] {
        &self.samples
    }

    pub fn latest(&self) -> Option<RatePoint> {
        self.samples.last().copied()
    }

    pub fn total_rx(&self) -> u64 {
        self.total_rx
    }

    pub fn total_tx(&self) -> u64 {
        self.total_tx
    }
}

/// Format a byte rate with automatic units (B/s, KB/s, MB/s, GB/s).
pub fn format_rate(bytes_per_sec: u64) -> String {
    const UNITS: [&str; 4] = ["B/s", "KB/s", "MB/s", "GB/s"];
    let mut value = bytes_per_sec as f64;
    let mut unit = 0;
    while value >= 1024.0 && unit < UNITS.len() - 1 {
        value /= 1024.0;
        unit += 1;
    }
    if unit == 0 {
        format!("{value:.0} {}", UNITS[unit])
    } else {
        format!("{value:.1} {}", UNITS[unit])
    }
}

/// Format a byte total with automatic units.
pub fn format_bytes(bytes: u64) -> String {
    const UNITS: [&str; 5] = ["B", "KB", "MB", "GB", "TB"];
    let mut value = bytes as f64;
    let mut unit = 0;
    while value >= 1024.0 && unit < UNITS.len() - 1 {
        value /= 1024.0;
        unit += 1;
    }
    if unit == 0 {
        format!("{value:.0} {}", UNITS[unit])
    } else {
        format!("{value:.1} {}", UNITS[unit])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rate_from_cumulative_diff() {
        // Rates come from differencing cumulative totals over the snapshot
        // interval, not from the core's per-period deltas.
        let mut h = TrafficHistory::new();
        h.feed(1000, 500, 1000);
        h.feed(2_501_000, 300_500, 2000); // +2.5MB in / +300KB out over 1s
        let p = h.latest().unwrap();
        assert_eq!(p.rx_bytes_per_sec, 2_500_000);
        assert_eq!(p.tx_bytes_per_sec, 300_000);
    }

    #[test]
    fn rate_scales_with_interval() {
        // A 2s snapshot gap halves the rate for the same byte delta.
        let mut h = TrafficHistory::new();
        h.feed(0, 0, 1000);
        h.feed(2_000_000, 200_000, 3000); // 2MB over 2s -> 1MB/s
        let p = h.latest().unwrap();
        assert_eq!(p.rx_bytes_per_sec, 1_000_000);
        assert_eq!(p.tx_bytes_per_sec, 100_000);
    }

    #[test]
    fn totals_track_latest_cumulative() {
        let mut h = TrafficHistory::new();
        h.feed(100, 50, 1000);
        h.feed(300, 110, 2000);
        h.feed(600, 180, 3000);
        assert_eq!(h.total_rx(), 600);
        assert_eq!(h.total_tx(), 180);
    }

    #[test]
    fn reset_on_clock_jump() {
        let mut h = TrafficHistory::new();
        h.feed(0, 0, 1000);
        h.feed(10_000, 0, 70_000); // gap > 60s -> fresh window
        assert!(h.latest().is_none());
        assert!(h.samples().is_empty());
    }

    #[test]
    fn capacity_bounded() {
        let mut h = TrafficHistory::new();
        for i in 0..SAMPLE_CAPACITY + 50 {
            h.feed(i as u64, 0, 1000 + i as u64);
        }
        assert_eq!(h.samples().len(), SAMPLE_CAPACITY);
    }

    #[test]
    fn byte_totals_use_automatic_units() {
        assert_eq!(format_bytes(0), "0 B");
        assert_eq!(format_bytes(1024), "1.0 KB");
        assert_eq!(format_bytes(84_844 * 1024), "82.9 MB");
        assert_eq!(format_bytes(632_525 * 1024), "617.7 MB");
    }
}

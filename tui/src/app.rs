//! Application state machine: page, selection and filters.

use std::collections::VecDeque;
use std::time::{Duration, Instant};

use crate::core::traffic::TrafficHistory;
use crate::rpc::schema::Snapshot;

pub const REFRESH_INTERVAL: Duration = Duration::from_secs(1);
pub const LOG_CAPACITY: usize = 5000;

/// One log line received from the core (event:log).
#[derive(Debug, Clone)]
pub struct LogEntry {
    pub seq: u64,
    pub level: String,
    pub line: String,
    pub timestamp_ms: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Page {
    Overview,
    Network,
    Servers,
    Logs,
    Routes,
}

impl Page {
    pub fn index(self) -> usize {
        match self {
            Page::Overview => 0,
            Page::Network => 1,
            Page::Servers => 2,
            Page::Logs => 3,
            Page::Routes => 4,
        }
    }

    pub fn from_index(index: usize) -> Self {
        match index % 5 {
            0 => Page::Overview,
            1 => Page::Network,
            2 => Page::Servers,
            3 => Page::Logs,
            _ => Page::Routes,
        }
    }
}

pub struct App {
    pub page: Page,
    pub snapshot: Option<Snapshot>,
    pub traffic: TrafficHistory,
    pub server_selection: usize,
    /// Active filter string (Servers page).
    pub filter: Option<String>,
    pub filter_active: bool,
    /// Log ring buffer (capacity LOG_CAPACITY) + follow/scroll state.
    pub logs: VecDeque<LogEntry>,
    pub logs_follow: bool,
    pub logs_scroll: usize,
    pub connecting: bool,
    pub error: Option<String>,
    pub last_refresh: Instant,
    pub quit_requested: bool,
    /// Retained for compatibility with the optional in-app mouse handlers;
    /// the terminal owns native text selection when mouse capture is off.
    pub mouse_enabled: bool,
    /// Help overlay open.
    pub help_open: bool,
}

impl Default for App {
    fn default() -> Self {
        Self::new()
    }
}

impl App {
    pub fn new() -> Self {
        Self {
            page: Page::Overview,
            snapshot: None,
            traffic: TrafficHistory::new(),
            server_selection: 0,
            filter: None,
            filter_active: false,
            logs: VecDeque::with_capacity(LOG_CAPACITY),
            logs_follow: true,
            logs_scroll: 0,
            connecting: false,
            error: None,
            last_refresh: Instant::now() - REFRESH_INTERVAL,
            quit_requested: false,
            mouse_enabled: true,
            help_open: false,
        }
    }

    /// Apply a fresh snapshot to the state model.
    pub fn apply_snapshot(&mut self, snapshot: Snapshot) {
        // Reset the traffic history when the core restarted (its monotonic
        // tick restarts from zero, so the new generation is smaller).
        if let Some(prev) = &self.snapshot {
            if snapshot.generation < prev.generation {
                self.traffic.reset();
            }
        }
        self.traffic.feed(
            snapshot.traffic.in_bytes,
            snapshot.traffic.out_bytes,
            snapshot.monotonic_ms,
        );

        let outbound_count = snapshot
            .outbounds
            .iter()
            .filter(|o| o.server_menu || o.tag.eq_ignore_ascii_case("main"))
            .count();
        self.snapshot = Some(snapshot);
        if self.server_selection >= outbound_count && outbound_count > 0 {
            self.server_selection = outbound_count - 1;
        }
        self.error = None;
    }

    /// Move the SERVERS selection by `delta`.
    pub fn move_selection(&mut self, delta: i64) {
        let count = self.visible_outbounds().len();
        if count == 0 {
            return;
        }
        let len = count as i64;
        self.server_selection =
            ((((self.server_selection as i64 + delta) % len) + len) % len) as usize;
    }

    /// Indices into `snapshot.outbounds` that pass the active filter.
    pub fn visible_outbounds(&self) -> Vec<usize> {
        let Some(snapshot) = &self.snapshot else {
            return Vec::new();
        };
        let filter = self.filter.as_deref().unwrap_or("").to_lowercase();
        snapshot
            .outbounds
            .iter()
            .enumerate()
            .filter(|(_, o)| {
                (o.server_menu || o.tag.eq_ignore_ascii_case("main"))
                    && (filter.is_empty()
                        || o.tag.to_lowercase().contains(&filter)
                        || o.display_name.to_lowercase().contains(&filter)
                        || o.server.to_lowercase().contains(&filter)
                        || o.current_entry.to_lowercase().contains(&filter)
                        || o.ranked_first_entry.to_lowercase().contains(&filter)
                        || o.probe_entry.to_lowercase().contains(&filter))
            })
            .map(|(i, _)| i)
            .collect()
    }

    /// Return the RPC action for the currently selected outbound.
    /// Switching is deliberately immediate; the core already validates the
    /// tag and reports whether it accepted the request.
    pub fn selected_switch(&self) -> Option<(String, serde_json::Value)> {
        let Some(snapshot) = &self.snapshot else {
            return None;
        };
        let visible = self.visible_outbounds();
        let Some(&index) = visible.get(self.server_selection) else {
            return None;
        };
        let outbound = &snapshot.outbounds[index];
        if outbound.active {
            Some((
                "switch_rank1".to_string(),
                serde_json::json!({ "tag": outbound.tag }),
            ))
        } else {
            Some((
                "switch_server".to_string(),
                serde_json::json!({ "tag": outbound.tag }),
            ))
        }
    }

    /// Append a log line; maintains the ring buffer and follow state.
    pub fn push_log(&mut self, entry: LogEntry) {
        if self.logs.len() >= LOG_CAPACITY {
            self.logs.pop_front();
        }
        self.logs.push_back(entry);
        if self.logs_follow {
            self.logs_scroll = 0;
        }
    }

    /// Scroll the log view by `delta` lines (positive = up/older).
    pub fn scroll_logs(&mut self, delta: i64) {
        if delta != 0 {
            self.logs_follow = false;
        }
        let max = self.logs.len().saturating_sub(1) as i64;
        self.logs_scroll = ((self.logs_scroll as i64 + delta).max(0).min(max)) as usize;
        if self.logs_scroll == 0 {
            self.logs_follow = true;
        }
    }
}

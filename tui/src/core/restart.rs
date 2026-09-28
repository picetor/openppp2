//! Shared policy for relaunching an owned core after its executor exits.

/// Planned restarts preserve the existing crash-recovery budget. Only an
/// unexpected exit consumes one of the three automatic recovery attempts.
pub fn allow_restart_after_exit(planned: bool, recovery_attempts: &mut u32) -> bool {
    if planned {
        return true;
    }
    *recovery_attempts = recovery_attempts.saturating_add(1);
    *recovery_attempts <= 3
}

#[cfg(test)]
mod tests {
    use super::allow_restart_after_exit;

    #[test]
    fn repeated_planned_restarts_preserve_recovery_budget() {
        let mut attempts = 2;
        for _ in 0..10 {
            assert!(allow_restart_after_exit(true, &mut attempts));
        }
        assert_eq!(attempts, 2);
        assert!(allow_restart_after_exit(false, &mut attempts));
        assert!(!allow_restart_after_exit(false, &mut attempts));
        assert_eq!(attempts, 4);
    }

    #[test]
    fn unexpected_exits_stop_after_three_recovery_attempts() {
        let mut attempts = 0;
        for _ in 0..3 {
            assert!(allow_restart_after_exit(false, &mut attempts));
        }
        assert!(!allow_restart_after_exit(false, &mut attempts));
        assert!(allow_restart_after_exit(true, &mut attempts));
        assert_eq!(attempts, 4);
        attempts = u32::MAX;
        assert!(!allow_restart_after_exit(false, &mut attempts));
    }
}

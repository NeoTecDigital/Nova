use std::time::{Duration, Instant};
use std::collections::VecDeque;
use serde::{Serialize, Deserialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScheduledTask {
    pub id: String,
    pub name: String,
    pub interval_ms: u64,
    pub repeat: bool,
    pub action_type: String,
    pub payload: serde_json::Value,
    #[serde(skip, default = "Instant::now")]
    pub next_run: Instant,
}

pub struct Scheduler {
    tasks: Vec<ScheduledTask>,
    pending_queue: VecDeque<ScheduledTask>,
    last_tick: Instant,
    tick_count: u64,
}

impl Default for Scheduler {
    fn default() -> Self {
        Self::new()
    }
}

impl Scheduler {
    pub fn new() -> Self {
        Self {
            tasks: Vec::new(),
            pending_queue: VecDeque::new(),
            last_tick: Instant::now(),
            tick_count: 0,
        }
    }

    pub fn schedule_recurring(&mut self, name: &str, interval_ms: u64, action_type: &str, payload: serde_json::Value) -> String {
        let id = uuid::Uuid::new_v4().to_string();
        let task = ScheduledTask {
            id: id.clone(),
            name: name.to_string(),
            interval_ms,
            repeat: true,
            action_type: action_type.to_string(),
            payload,
            next_run: Instant::now() + Duration::from_millis(interval_ms),
        };
        self.tasks.push(task);
        id
    }

    pub fn schedule_delayed(&mut self, name: &str, delay_ms: u64, action_type: &str, payload: serde_json::Value) -> String {
        let id = uuid::Uuid::new_v4().to_string();
        let task = ScheduledTask {
            id: id.clone(),
            name: name.to_string(),
            interval_ms: delay_ms,
            repeat: false,
            action_type: action_type.to_string(),
            payload,
            next_run: Instant::now() + Duration::from_millis(delay_ms),
        };
        self.tasks.push(task);
        id
    }

    pub fn update(&mut self, now: Instant) -> Vec<ScheduledTask> {
        self.tick_count += 1;
        self.last_tick = now;
        let mut ready = Vec::new();

        let mut i = 0;
        while i < self.tasks.len() {
            if now >= self.tasks[i].next_run {
                ready.push(self.tasks[i].clone());
                if self.tasks[i].repeat {
                    self.tasks[i].next_run = now + Duration::from_millis(self.tasks[i].interval_ms);
                    i += 1;
                } else {
                    self.tasks.remove(i);
                }
            } else {
                i += 1;
            }
        }

        ready
    }

    pub fn task_count(&self) -> usize {
        self.tasks.len()
    }

    pub fn tick_count(&self) -> u64 {
        self.tick_count
    }
}

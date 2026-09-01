use oats_framework::systems::Priority;
use serde::{Serialize, Deserialize};
use std::collections::HashMap;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub enum ExecutionStage {
    PreUpdate = 0,
    Update = 1,
    BusinessLogic = 2,
    PostUpdate = 3,
    StateSync = 4,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SystemRegistration {
    pub name: String,
    pub stage: ExecutionStage,
    pub priority: Priority,
    pub dependencies: Vec<String>,
}

pub struct Coordinator {
    registered_systems: HashMap<String, SystemRegistration>,
    execution_order: Vec<String>,
}

impl Default for Coordinator {
    fn default() -> Self {
        Self::new()
    }
}

impl Coordinator {
    pub fn new() -> Self {
        Self {
            registered_systems: HashMap::new(),
            execution_order: Vec::new(),
        }
    }

    pub fn register(&mut self, name: &str, stage: ExecutionStage, priority: Priority, dependencies: Vec<String>) {
        let reg = SystemRegistration {
            name: name.to_string(),
            stage,
            priority,
            dependencies,
        };
        self.registered_systems.insert(name.to_string(), reg);
        self.rebuild_order();
    }

    pub fn get_execution_order(&self) -> &[String] {
        &self.execution_order
    }

    pub fn get_stage_systems(&self, stage: ExecutionStage) -> Vec<String> {
        let mut list: Vec<_> = self.registered_systems
            .values()
            .filter(|r| r.stage == stage)
            .cloned()
            .collect();
        list.sort_by(|a, b| b.priority.cmp(&a.priority));
        list.into_iter().map(|r| r.name).collect()
    }

    fn rebuild_order(&mut self) {
        let mut all: Vec<_> = self.registered_systems.values().cloned().collect();
        all.sort_by(|a, b| {
            a.stage.cmp(&b.stage).then_with(|| b.priority.cmp(&a.priority))
        });
        self.execution_order = all.into_iter().map(|r| r.name).collect();
    }
}

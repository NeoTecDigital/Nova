use oats_framework::{Action, ActionContext, ActionResult, Object, OatsError};
use std::collections::HashMap;
use std::sync::Arc;
use tokio::sync::mpsc;
use serde::{Serialize, Deserialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ActionDispatchJob {
    pub id: String,
    pub action_type: String,
    pub target_object_id: Option<String>,
    pub payload: serde_json::Value,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DispatcherStats {
    pub total_dispatched: u64,
    pub total_completed: u64,
    pub total_failed: u64,
}

pub struct Dispatcher {
    action_registry: HashMap<String, Arc<dyn Action + Send + Sync>>,
    stats: DispatcherStats,
}

impl Default for Dispatcher {
    fn default() -> Self {
        Self::new()
    }
}

impl Dispatcher {
    pub fn new() -> Self {
        Self {
            action_registry: HashMap::new(),
            stats: DispatcherStats {
                total_dispatched: 0,
                total_completed: 0,
                total_failed: 0,
            },
        }
    }

    pub fn register_action(&mut self, action_name: &str, action: Arc<dyn Action + Send + Sync>) {
        self.action_registry.insert(action_name.to_string(), action);
    }

    pub async fn dispatch(
        &mut self,
        action_name: &str,
        context: ActionContext,
    ) -> Result<ActionResult, OatsError> {
        self.stats.total_dispatched += 1;
        let action = self.action_registry.get(action_name).ok_or_else(|| {
            self.stats.total_failed += 1;
            OatsError::action_failed(format!("Action '{}' not registered in Dispatcher", action_name))
        })?;

        match action.execute(context).await {
            Ok(result) => {
                self.stats.total_completed += 1;
                Ok(result)
            }
            Err(e) => {
                self.stats.total_failed += 1;
                Err(e)
            }
        }
    }

    pub fn stats(&self) -> &DispatcherStats {
        &self.stats
    }
}

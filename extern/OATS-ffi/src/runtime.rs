use crate::scheduler::Scheduler;
use crate::coordinator::{Coordinator, ExecutionStage};
use crate::dispatcher::Dispatcher;
use crate::organizer::{Organizer, StateDelta};
use crate::registry::TypeRegistry;
use oats_framework::{Object, Trait, TraitData, Priority};
use tokio::runtime::Runtime;
use std::time::Instant;

pub struct OatsRuntime {
    pub type_registry: TypeRegistry,
    pub scheduler: Scheduler,
    pub coordinator: Coordinator,
    pub dispatcher: Dispatcher,
    pub organizer: Organizer,
    
    pub tokio_runtime: Runtime,
}

impl Default for OatsRuntime {
    fn default() -> Self {
        Self::new()
    }
}

impl OatsRuntime {
    pub fn new() -> Self {
        let tokio_runtime = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .expect("Failed to initialize Tokio runtime for OATS-ffi");

        let mut coordinator = Coordinator::new();
        coordinator.register("filesystem_sync", ExecutionStage::PreUpdate, Priority::High, vec![]);
        coordinator.register("spatial_physics_sync", ExecutionStage::Update, Priority::Normal, vec!["filesystem_sync".to_string()]);
        coordinator.register("telemetry_sync", ExecutionStage::PostUpdate, Priority::Low, vec!["spatial_physics_sync".to_string()]);

        let mut scheduler = Scheduler::new();
        // Schedule periodic real-world sync tasks
        scheduler.schedule_recurring("fs_sync_pulse", 2000, "sync_filesystem", serde_json::json!({}));
        scheduler.schedule_recurring("physics_sync_pulse", 1000, "sync_physics", serde_json::json!({}));
        scheduler.schedule_recurring("telemetry_pulse", 500, "capture_telemetry", serde_json::json!({}));

        Self {
            type_registry: TypeRegistry::new(),
            scheduler,
            coordinator,
            dispatcher: Dispatcher::new(),
            organizer: Organizer::new(),
            tokio_runtime,
        }
    }

    /// Register a real FileSystem entity (file or directory) into OATS
    pub fn register_filesystem_entity(&mut self, path: &str, is_dir: bool, size_bytes: u64, ext: &str) -> String {
        let mut obj = Object::new(path, "FileSystemEntity");
        obj.add_trait(Trait::new("path", TraitData::String(path.to_string())));
        obj.add_trait(Trait::new("is_directory", TraitData::Boolean(is_dir)));
        obj.add_trait(Trait::new("size_bytes", TraitData::Number(size_bytes as f64)));
        obj.add_trait(Trait::new("extension", TraitData::String(ext.to_string())));

        let id = obj.id().to_string();
        self.organizer.insert_object(obj);
        self.organizer.record_event("ENTITY_REGISTERED", &format!("Registered FS entity: {}", path));
        id
    }

    /// Register a 3D Spatial Pill entity into OATS
    pub fn register_spatial_pill(&mut self, name: &str, path: &str, pos: [f32; 3], rot: [f32; 4], radius: f32, height: f32) -> String {
        let mut obj = Object::new(name, "SpatialPill");
        obj.add_trait(Trait::new("path", TraitData::String(path.to_string())));
        obj.add_trait(Trait::new("radius", TraitData::Number(radius as f64)));
        obj.add_trait(Trait::new("height", TraitData::Number(height as f64)));
        obj.add_trait(Trait::new("pos_x", TraitData::Number(pos[0] as f64)));
        obj.add_trait(Trait::new("pos_y", TraitData::Number(pos[1] as f64)));
        obj.add_trait(Trait::new("pos_z", TraitData::Number(pos[2] as f64)));
        obj.add_trait(Trait::new("rot_w", TraitData::Number(rot[0] as f64)));
        obj.add_trait(Trait::new("rot_x", TraitData::Number(rot[1] as f64)));
        obj.add_trait(Trait::new("rot_y", TraitData::Number(rot[2] as f64)));
        obj.add_trait(Trait::new("rot_z", TraitData::Number(rot[3] as f64)));

        let id = obj.id().to_string();
        self.organizer.insert_object(obj);
        id
    }

    /// Register a Lumberjack Hypergraph DAG node into OATS
    pub fn register_hypergraph_node(&mut self, node_id: &str, namespace: &str, parents_json: &str) -> String {
        let mut obj = Object::new(node_id, "HypergraphDAGNode");
        obj.add_trait(Trait::new("node_id", TraitData::String(node_id.to_string())));
        obj.add_trait(Trait::new("namespace", TraitData::String(namespace.to_string())));
        obj.add_trait(Trait::new("parents", TraitData::String(parents_json.to_string())));

        let id = obj.id().to_string();
        self.organizer.insert_object(obj);
        self.organizer.record_event("DAG_NODE_LINKED", &format!("Linked Hypergraph Node: {} in {}", node_id, namespace));
        id
    }

    /// Update spatial pose for an existing object
    pub fn update_spatial_pose(&mut self, id: &str, pos: [f32; 3], rot: [f32; 4]) {
        self.organizer.update_trait(id, Trait::new("pos_x", TraitData::Number(pos[0] as f64)));
        self.organizer.update_trait(id, Trait::new("pos_y", TraitData::Number(pos[1] as f64)));
        self.organizer.update_trait(id, Trait::new("pos_z", TraitData::Number(pos[2] as f64)));
        self.organizer.update_trait(id, Trait::new("rot_w", TraitData::Number(rot[0] as f64)));
        self.organizer.update_trait(id, Trait::new("rot_x", TraitData::Number(rot[1] as f64)));
        self.organizer.update_trait(id, Trait::new("rot_y", TraitData::Number(rot[2] as f64)));
        self.organizer.update_trait(id, Trait::new("rot_z", TraitData::Number(rot[3] as f64)));
    }

    /// Step the agnostic ECS runtime
    pub fn step(&mut self, delta_time_secs: f64) -> StateDelta {
        let due_tasks = self.scheduler.update(Instant::now());
        for task in due_tasks {
            self.organizer.record_event("SCHEDULED_TASK", &format!("Executed task: {}", task.name));
        }

        self.organizer.flush_delta(delta_time_secs)
    }

    pub fn get_types_json(&self) -> String {
        let types = self.type_registry.get_all_types();
        serde_json::to_string(&types).unwrap_or_else(|_| "[]".to_string())
    }

    pub fn get_entities_json(&self, obj_type: Option<&str>) -> String {
        let objects: Vec<&Object> = if let Some(t) = obj_type {
            self.organizer.query_by_type(t)
        } else {
            self.organizer.all_objects()
        };

        let json_arr: Vec<serde_json::Value> = objects.iter().map(|o| {
            serde_json::json!({
                "id": o.id().to_string(),
                "name": o.name(),
                "type": o.object_type(),
                "traits": o.traits().iter().map(|(k, v)| (k.clone(), format!("{:?}", v.data()))).collect::<std::collections::HashMap<_, _>>()
            })
        }).collect();

        serde_json::to_string(&json_arr).unwrap_or_else(|_| "[]".to_string())
    }
}

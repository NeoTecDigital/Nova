use oats_framework::{System, Priority, ActionResult, OatsError, Object};
use async_trait::async_trait;
use serde::{Serialize, Deserialize};
use std::collections::HashMap;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FieldSchema {
    pub name: String,
    pub data_type: String, // "string", "number", "boolean", "vec3", "vec4", "json"
    pub description: String,
    pub is_required: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TypeDefinition {
    pub type_name: String,
    pub description: String,
    pub fields: Vec<FieldSchema>,
}

#[derive(Default)]
pub struct TypeRegistry {
    types: HashMap<String, TypeDefinition>,
}

impl TypeRegistry {
    pub fn new() -> Self {
        let mut reg = Self {
            types: HashMap::new(),
        };
        reg.register_builtin_types();
        reg
    }

    pub fn register_type(&mut self, type_def: TypeDefinition) {
        self.types.insert(type_def.type_name.clone(), type_def);
    }

    pub fn get_type(&self, type_name: &str) -> Option<&TypeDefinition> {
        self.types.get(type_name)
    }

    pub fn get_all_types(&self) -> Vec<TypeDefinition> {
        self.types.values().cloned().collect()
    }

    fn register_builtin_types(&mut self) {
        // 1. Real FileSystem Entity
        self.register_type(TypeDefinition {
            type_name: "FileSystemEntity".to_string(),
            description: "Physical file or directory entity in the host workspace".to_string(),
            fields: vec![
                FieldSchema { name: "path".to_string(), data_type: "string".to_string(), description: "Absolute path on disk".to_string(), is_required: true },
                FieldSchema { name: "is_directory".to_string(), data_type: "boolean".to_string(), description: "True if directory hub".to_string(), is_required: true },
                FieldSchema { name: "size_bytes".to_string(), data_type: "number".to_string(), description: "File size in bytes".to_string(), is_required: true },
                FieldSchema { name: "extension".to_string(), data_type: "string".to_string(), description: "File extension (.rs, .cpp, .h)".to_string(), is_required: false },
                FieldSchema { name: "modified_unix".to_string(), data_type: "number".to_string(), description: "Last modified epoch timestamp".to_string(), is_required: false },
            ],
        });

        // 2. 3D Spatial Pill Component
        self.register_type(TypeDefinition {
            type_name: "SpatialPill".to_string(),
            description: "3D Parametric capsule representation in Quaternionic space".to_string(),
            fields: vec![
                FieldSchema { name: "position".to_string(), data_type: "vec3".to_string(), description: "World XYZ coordinates".to_string(), is_required: true },
                FieldSchema { name: "orientation".to_string(), data_type: "vec4".to_string(), description: "Quaternionic orientation [w,x,y,z]".to_string(), is_required: true },
                FieldSchema { name: "radius".to_string(), data_type: "number".to_string(), description: "Capsule radius".to_string(), is_required: true },
                FieldSchema { name: "height".to_string(), data_type: "number".to_string(), description: "Cylinder height".to_string(), is_required: true },
                FieldSchema { name: "color".to_string(), data_type: "vec4".to_string(), description: "RGBA base color".to_string(), is_required: true },
            ],
        });

        // 3. Engine Physics State
        self.register_type(TypeDefinition {
            type_name: "EnginePhysicsState".to_string(),
            description: "Non-linear complex phase dynamics & spatial acceleration parameters".to_string(),
            fields: vec![
                FieldSchema { name: "coupling_strength".to_string(), data_type: "number".to_string(), description: "Phase coupling lambda".to_string(), is_required: true },
                FieldSchema { name: "phase_velocity".to_string(), data_type: "number".to_string(), description: "Phase velocity omega in rad/s".to_string(), is_required: true },
                FieldSchema { name: "dither_enabled".to_string(), data_type: "boolean".to_string(), description: "Temporal sub-pixel dithering".to_string(), is_required: true },
                FieldSchema { name: "laser_precision".to_string(), data_type: "number".to_string(), description: "Pinpoint raycast precision".to_string(), is_required: true },
            ],
        });

        // 4. Lumberjack Hypergraph DAG Node
        self.register_type(TypeDefinition {
            type_name: "HypergraphDAGNode".to_string(),
            description: "Multi-parent hypergraph forest node with JIT cache recall".to_string(),
            fields: vec![
                FieldSchema { name: "node_id".to_string(), data_type: "string".to_string(), description: "Unique URN / Node ID".to_string(), is_required: true },
                FieldSchema { name: "namespace".to_string(), data_type: "string".to_string(), description: "Virtual memory namespace".to_string(), is_required: true },
                FieldSchema { name: "parents".to_string(), data_type: "json".to_string(), description: "Map of parent URNs to relationship types".to_string(), is_required: true },
                FieldSchema { name: "entries_count".to_string(), data_type: "number".to_string(), description: "Total entries in history log".to_string(), is_required: true },
            ],
        });
    }
}

// ---------------------------------------------------------------------------
// Core Agnostic Systems
// ---------------------------------------------------------------------------

#[derive(Default)]
pub struct FilesystemSyncSystem;

#[async_trait]
impl System for FilesystemSyncSystem {
    fn name(&self) -> &'static str {
        "filesystem_sync_system"
    }

    fn description(&self) -> &'static str {
        "Agnostic filesystem state reconciler"
    }

    async fn process(&mut self, _objects: Vec<Object>, _priority: Priority) -> Result<Vec<ActionResult>, OatsError> {
        Ok(Vec::new())
    }
}

#[derive(Default)]
pub struct SpatialPhysicsSyncSystem;

#[async_trait]
impl System for SpatialPhysicsSyncSystem {
    fn name(&self) -> &'static str {
        "spatial_physics_sync_system"
    }

    fn description(&self) -> &'static str {
        "Agnostic 3D spatial phase field evolution system"
    }

    async fn process(&mut self, _objects: Vec<Object>, _priority: Priority) -> Result<Vec<ActionResult>, OatsError> {
        Ok(Vec::new())
    }
}

#[derive(Default)]
pub struct TelemetrySystem;

#[async_trait]
impl System for TelemetrySystem {
    fn name(&self) -> &'static str {
        "telemetry_system"
    }

    fn description(&self) -> &'static str {
        "Agnostic telemetry and metrics aggregation system"
    }

    async fn process(&mut self, _objects: Vec<Object>, _priority: Priority) -> Result<Vec<ActionResult>, OatsError> {
        Ok(Vec::new())
    }
}

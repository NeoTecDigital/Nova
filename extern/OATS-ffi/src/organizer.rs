use oats_framework::{Object, Trait, TraitData};
use std::collections::{HashMap, HashSet};
use serde::{Serialize, Deserialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TraitUpdateDelta {
    pub object_id: String,
    pub object_name: String,
    pub trait_name: String,
    pub data: serde_json::Value,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SystemEventDelta {
    pub event_type: String,
    pub message: String,
    pub timestamp_utc: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct StateDelta {
    pub tick: u64,
    pub delta_time_secs: f64,
    pub added_objects: Vec<serde_json::Value>,
    pub removed_object_ids: Vec<String>,
    pub updated_traits: Vec<TraitUpdateDelta>,
    pub events: Vec<SystemEventDelta>,
}

pub struct Organizer {
    objects_by_id: HashMap<String, Object>,
    trait_indices: HashMap<String, HashSet<String>>, // trait_name -> Set<object_id>
    type_indices: HashMap<String, HashSet<String>>,  // object_type -> Set<object_id>
    
    current_delta: StateDelta,
    tick_counter: u64,
}

impl Default for Organizer {
    fn default() -> Self {
        Self::new()
    }
}

impl Organizer {
    pub fn new() -> Self {
        Self {
            objects_by_id: HashMap::new(),
            trait_indices: HashMap::new(),
            type_indices: HashMap::new(),
            current_delta: StateDelta::default(),
            tick_counter: 0,
        }
    }

    pub fn insert_object(&mut self, object: Object) {
        let id = object.id().to_string();
        let obj_type = object.object_type().to_string();

        self.type_indices.entry(obj_type).or_default().insert(id.clone());

        for (trait_name, _) in object.traits() {
            self.trait_indices.entry(trait_name.clone()).or_default().insert(id.clone());
        }

        let json_val = serde_json::json!({
            "id": id,
            "name": object.name(),
            "type": object.object_type(),
            "traits": object.traits().iter().map(|(k, v)| {
                (k.clone(), match v.data() {
                    TraitData::String(s) => serde_json::json!(s),
                    TraitData::Number(n) => serde_json::json!(n),
                    TraitData::Boolean(b) => serde_json::json!(b),
                    TraitData::Object(map) => serde_json::json!(map),
                    TraitData::Array(arr) => serde_json::json!(arr),
                    TraitData::Binary(bin) => serde_json::json!(bin),
                })
            }).collect::<serde_json::Map<String, serde_json::Value>>()
        });

        self.current_delta.added_objects.push(json_val);
        self.objects_by_id.insert(id, object);
    }

    pub fn remove_object(&mut self, id: &str) -> Option<Object> {
        if let Some(obj) = self.objects_by_id.remove(id) {
            if let Some(set) = self.type_indices.get_mut(obj.object_type()) {
                set.remove(id);
            }
            for (trait_name, _) in obj.traits() {
                if let Some(set) = self.trait_indices.get_mut(trait_name) {
                    set.remove(id);
                }
            }
            self.current_delta.removed_object_ids.push(id.to_string());
            Some(obj)
        } else {
            None
        }
    }

    pub fn update_trait(&mut self, object_id: &str, new_trait: Trait) {
        if let Some(obj) = self.objects_by_id.get_mut(object_id) {
            let trait_name = new_trait.name().to_string();
            let data_json = match new_trait.data() {
                TraitData::String(s) => serde_json::json!(s),
                TraitData::Number(n) => serde_json::json!(n),
                TraitData::Boolean(b) => serde_json::json!(b),
                TraitData::Object(map) => serde_json::json!(map),
                TraitData::Array(arr) => serde_json::json!(arr),
                TraitData::Binary(bin) => serde_json::json!(bin),
            };

            self.trait_indices.entry(trait_name.clone()).or_default().insert(object_id.to_string());

            self.current_delta.updated_traits.push(TraitUpdateDelta {
                object_id: object_id.to_string(),
                object_name: obj.name().to_string(),
                trait_name: trait_name.clone(),
                data: data_json,
            });

            obj.add_trait(new_trait);
        }
    }

    pub fn record_event(&mut self, event_type: &str, message: &str) {
        self.current_delta.events.push(SystemEventDelta {
            event_type: event_type.to_string(),
            message: message.to_string(),
            timestamp_utc: chrono::Utc::now().to_rfc3339(),
        });
    }

    pub fn query_by_type(&self, object_type: &str) -> Vec<&Object> {
        self.type_indices.get(object_type)
            .map(|ids| ids.iter().filter_map(|id| self.objects_by_id.get(id)).collect())
            .unwrap_or_default()
    }

    pub fn query_with_traits(&self, trait_names: &[&str]) -> Vec<&Object> {
        if trait_names.is_empty() {
            return self.objects_by_id.values().collect();
        }

        let mut candidate_ids: Option<HashSet<String>> = None;
        for &t in trait_names {
            if let Some(ids) = self.trait_indices.get(t) {
                if let Some(ref mut c) = candidate_ids {
                    *c = c.intersection(ids).cloned().collect();
                } else {
                    candidate_ids = Some(ids.clone());
                }
            } else {
                return Vec::new();
            }
        }

        candidate_ids
            .map(|ids| ids.iter().filter_map(|id| self.objects_by_id.get(id)).collect())
            .unwrap_or_default()
    }

    pub fn get_object(&self, id: &str) -> Option<&Object> {
        self.objects_by_id.get(id)
    }

    pub fn get_object_mut(&mut self, id: &str) -> Option<&mut Object> {
        self.objects_by_id.get_mut(id)
    }

    pub fn all_objects(&self) -> Vec<&Object> {
        self.objects_by_id.values().collect()
    }

    pub fn flush_delta(&mut self, dt_secs: f64) -> StateDelta {
        self.tick_counter += 1;
        let mut delta = std::mem::take(&mut self.current_delta);
        delta.tick = self.tick_counter;
        delta.delta_time_secs = dt_secs;
        delta
    }

    pub fn object_count(&self) -> usize {
        self.objects_by_id.len()
    }
}

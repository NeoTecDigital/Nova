use oats_ffi::{OatsRuntime, Dispatcher, Organizer};
use oats_framework::{Object, Trait, TraitData, Action, ActionContext, ActionResult};
use async_trait::async_trait;
use std::time::Instant;
use std::sync::Arc;

struct HighLoadAction;

#[async_trait]
impl Action for HighLoadAction {
    fn name(&self) -> &str {
        "HighLoadAction"
    }

    fn description(&self) -> &str {
        "Benchmark high load action"
    }

    async fn execute(&self, _context: ActionContext) -> Result<ActionResult, oats_framework::OatsError> {
        let mut res = ActionResult::success();
        res.trait_updates.push(Trait::new("processed", TraitData::Boolean(true)));
        Ok(res)
    }
}

#[test]
fn test_seam_oats_massive_entity_ingestion() {
    println!("\n==========================================================================");
    println!(" [SEAM 3 BENCHMARK]: OATS Massive Entity Ingestion & Indexing Limits");
    println!("==========================================================================");
    let mut organizer = Organizer::new();
    let num_entities = 20_000;

    let start = Instant::now();
    for i in 0..num_entities {
        let mut obj = Object::new(format!("Entity_{}", i), "stress_node");
        obj.add_trait(Trait::new("index", TraitData::Number(i as f64)));
        obj.add_trait(Trait::new("status", TraitData::String(if i % 2 == 0 { "ACTIVE".into() } else { "IDLE".into() })));
        obj.add_trait(Trait::new("weight", TraitData::Number((i as f64) * 0.05)));
        organizer.insert_object(obj);
    }
    let duration = start.elapsed();
    let throughput = (num_entities as f64) / duration.as_secs_f64();
    println!(" [SEAM 3 RESULT] Ingested {} complex entities in {:?} ({:.0} entities/sec)", num_entities, duration, throughput);
    assert_eq!(organizer.object_count(), num_entities);

    // Trait query benchmark
    let q_start = Instant::now();
    let query_count = 1_000;
    for _ in 0..query_count {
        let res = organizer.query_by_type("stress_node");
        assert_eq!(res.len(), num_entities);
    }
    let q_duration = q_start.elapsed();
    println!(" [SEAM 3 RESULT] Executed {} archetype index lookups over {} entities in {:?} ({:.2} us/query)", 
        query_count, num_entities, q_duration, (q_duration.as_micros() as f64) / (query_count as f64));
}

#[test]
fn test_seam_oats_concurrent_dispatch_limits() {
    println!("\n==========================================================================");
    println!(" [SEAM 3 BENCHMARK]: Concurrent Async Action Dispatch Throughput Limits");
    println!("==========================================================================");
    let rt = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(4)
        .enable_all()
        .build()
        .unwrap();

    let mut dispatcher = Dispatcher::new();
    dispatcher.register_action("high_load", Arc::new(HighLoadAction));

    let num_actions = 20_000;
    let start = Instant::now();

    rt.block_on(async {
        for _ in 0..num_actions {
            let ctx = ActionContext::new();
            let res = dispatcher.dispatch("high_load", ctx).await;
            assert!(res.is_ok());
        }
    });

    let duration = start.elapsed();
    let throughput = (num_actions as f64) / duration.as_secs_f64();
    println!(" [SEAM 3 RESULT] Dispatched & executed {} async actions in {:?} ({:.0} actions/sec)", 
        num_actions, duration, throughput);
}

#[test]
fn test_seam_oats_runtime_delta_sync_limits() {
    println!("\n==========================================================================");
    println!(" [SEAM 3 BENCHMARK]: End-to-End Runtime Delta Generation & Serialization Limits");
    println!("==========================================================================");
    let mut runtime = OatsRuntime::new();

    // Register 1,000 real-world style filesystem entities & 3D pills
    for i in 0..1_000 {
        runtime.register_filesystem_entity(
            &format!("/workspace/file_{}.cpp", i),
            false,
            (i as u64) * 1024,
            ".cpp"
        );
        runtime.register_spatial_pill(
            &format!("Pill_{}", i),
            &format!("/workspace/file_{}.cpp", i),
            [(i as f32) * 0.1, (i as f32) * 0.2, -0.5],
            [1.0, 0.0, 0.0, 0.0],
            0.06,
            0.18
        );
    }

    let start = Instant::now();
    let num_steps = 100;
    for _ in 0..num_steps {
        let delta = runtime.step(0.016);
        let json_str = serde_json::to_string(&delta).unwrap();
        assert!(!json_str.is_empty());
    }
    let duration = start.elapsed();
    let step_fps = (num_steps as f64) / duration.as_secs_f64();
    println!(" [SEAM 3 RESULT] Completed {} full ECS steps + delta serializations (1,000 active nodes) in {:?} ({:.0} FPS / ECS Steps/sec)", 
        num_steps, duration, step_fps);
}

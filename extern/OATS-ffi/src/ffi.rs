use crate::runtime::OatsRuntime;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;

#[no_mangle]
pub extern "C" fn oats_runtime_create() -> *mut OatsRuntime {
    let runtime = Box::new(OatsRuntime::new());
    Box::into_raw(runtime)
}

#[no_mangle]
pub extern "C" fn oats_runtime_destroy(runtime: *mut OatsRuntime) {
    if !runtime.is_null() {
        unsafe {
            let _ = Box::from_raw(runtime);
        }
    }
}

#[no_mangle]
pub extern "C" fn oats_runtime_step(runtime: *mut OatsRuntime, delta_time_secs: f64) -> *mut c_char {
    if runtime.is_null() {
        return std::ptr::null_mut();
    }
    let runtime = unsafe { &mut *runtime };
    let delta = runtime.step(delta_time_secs);
    
    let json_str = serde_json::to_string(&delta).unwrap_or_else(|_| "{}".to_string());
    let c_str = CString::new(json_str).unwrap_or_default();
    c_str.into_raw()
}

#[no_mangle]
pub extern "C" fn oats_runtime_register_fs_entity(
    runtime: *mut OatsRuntime,
    path: *const c_char,
    is_dir: bool,
    size_bytes: u64,
    ext: *const c_char,
) -> *mut c_char {
    if runtime.is_null() || path.is_null() {
        return std::ptr::null_mut();
    }
    let runtime = unsafe { &mut *runtime };
    let path_str = unsafe { CStr::from_ptr(path).to_str().unwrap_or("") };
    let ext_str = if !ext.is_null() {
        unsafe { CStr::from_ptr(ext).to_str().unwrap_or("") }
    } else {
        ""
    };

    let id = runtime.register_filesystem_entity(path_str, is_dir, size_bytes, ext_str);
    let c_str = CString::new(id).unwrap_or_default();
    c_str.into_raw()
}

#[no_mangle]
pub extern "C" fn oats_runtime_register_spatial_pill(
    runtime: *mut OatsRuntime,
    name: *const c_char,
    path: *const c_char,
    pos_x: f32,
    pos_y: f32,
    pos_z: f32,
    rot_w: f32,
    rot_x: f32,
    rot_y: f32,
    rot_z: f32,
    radius: f32,
    height: f32,
) -> *mut c_char {
    if runtime.is_null() || name.is_null() || path.is_null() {
        return std::ptr::null_mut();
    }
    let runtime = unsafe { &mut *runtime };
    let name_str = unsafe { CStr::from_ptr(name).to_str().unwrap_or("") };
    let path_str = unsafe { CStr::from_ptr(path).to_str().unwrap_or("") };

    let id = runtime.register_spatial_pill(
        name_str,
        path_str,
        [pos_x, pos_y, pos_z],
        [rot_w, rot_x, rot_y, rot_z],
        radius,
        height,
    );
    let c_str = CString::new(id).unwrap_or_default();
    c_str.into_raw()
}

#[no_mangle]
pub extern "C" fn oats_runtime_register_hypergraph_node(
    runtime: *mut OatsRuntime,
    node_id: *const c_char,
    namespace: *const c_char,
    parents_json: *const c_char,
) -> *mut c_char {
    if runtime.is_null() || node_id.is_null() {
        return std::ptr::null_mut();
    }
    let runtime = unsafe { &mut *runtime };
    let node_str = unsafe { CStr::from_ptr(node_id).to_str().unwrap_or("") };
    let ns_str = if !namespace.is_null() {
        unsafe { CStr::from_ptr(namespace).to_str().unwrap_or("") }
    } else {
        ""
    };
    let parents_str = if !parents_json.is_null() {
        unsafe { CStr::from_ptr(parents_json).to_str().unwrap_or("{}") }
    } else {
        "{}"
    };

    let id = runtime.register_hypergraph_node(node_str, ns_str, parents_str);
    let c_str = CString::new(id).unwrap_or_default();
    c_str.into_raw()
}

#[no_mangle]
pub extern "C" fn oats_runtime_update_spatial_pose(
    runtime: *mut OatsRuntime,
    id: *const c_char,
    pos_x: f32,
    pos_y: f32,
    pos_z: f32,
    rot_w: f32,
    rot_x: f32,
    rot_y: f32,
    rot_z: f32,
) {
    if runtime.is_null() || id.is_null() {
        return;
    }
    let runtime = unsafe { &mut *runtime };
    let id_str = unsafe { CStr::from_ptr(id).to_str().unwrap_or("") };
    runtime.update_spatial_pose(id_str, [pos_x, pos_y, pos_z], [rot_w, rot_x, rot_y, rot_z]);
}

#[no_mangle]
pub extern "C" fn oats_runtime_get_types_json(runtime: *mut OatsRuntime) -> *mut c_char {
    if runtime.is_null() {
        return std::ptr::null_mut();
    }
    let runtime = unsafe { &*runtime };
    let json_str = runtime.get_types_json();
    let c_str = CString::new(json_str).unwrap_or_default();
    c_str.into_raw()
}

#[no_mangle]
pub extern "C" fn oats_runtime_get_entities_json(runtime: *mut OatsRuntime, obj_type: *const c_char) -> *mut c_char {
    if runtime.is_null() {
        return std::ptr::null_mut();
    }
    let runtime = unsafe { &*runtime };
    let filter = if !obj_type.is_null() {
        let s = unsafe { CStr::from_ptr(obj_type).to_str().unwrap_or("") };
        if s.is_empty() { None } else { Some(s) }
    } else {
        None
    };

    let json_str = runtime.get_entities_json(filter);
    let c_str = CString::new(json_str).unwrap_or_default();
    c_str.into_raw()
}

#[no_mangle]
pub extern "C" fn oats_runtime_free_string(s: *mut c_char) {
    if !s.is_null() {
        unsafe {
            let _ = CString::from_raw(s);
        }
    }
}

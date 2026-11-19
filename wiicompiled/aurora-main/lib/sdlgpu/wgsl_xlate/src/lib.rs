//! WGSL -> MSL for aurora's SDL_GPU backend, with resources renumbered to SDL_GPU's slot rules.
//!
//! The output is one text blob: binding-map lines, a `---` separator, then the shader source.
//!   entry <translated entry point name>
//!   counts <samplers> <storage textures> <storage buffers> <uniform buffers> <rw storage textures> <rw storage buffers>
//!   workgroup <x> <y> <z>                      (compute only)
//!   bind <group> <binding> <kind> <slot> <bytes>  (bytes: a uniform's size, else 0)
//!   sizes <slot> <group> <binding>...          (naga's hidden runtime-array sizes buffer, when needed)
//! kinds: uniform, storage, rwstorage, texture, storagetexture, rwstoragetexture, sampler (a sampler's slot is the
//! slot of the texture it is paired with).

use std::collections::{BTreeMap, BTreeSet};
use std::ffi::{c_char, CStr};
use std::fmt::Write as _;

use naga::back::msl;
use naga::valid::{Capabilities, ValidationFlags, Validator};
use naga::{AddressSpace, Expression, Handle, ImageClass, ResourceBinding, ShaderStage, StorageAccess, TypeInner};

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
enum Kind {
    Uniform,
    Storage,
    RwStorage,
    Texture,
    StorageTexture,
    RwStorageTexture,
    Sampler,
}

impl Kind {
    fn name(self) -> &'static str {
        match self {
            Kind::Uniform => "uniform",
            Kind::Storage => "storage",
            Kind::RwStorage => "rwstorage",
            Kind::Texture => "texture",
            Kind::StorageTexture => "storagetexture",
            Kind::RwStorageTexture => "rwstoragetexture",
            Kind::Sampler => "sampler",
        }
    }
}

fn classify(module: &naga::Module, var: &naga::GlobalVariable) -> Option<Kind> {
    match var.space {
        AddressSpace::Uniform => Some(Kind::Uniform),
        AddressSpace::Storage { access } => {
            if access.contains(StorageAccess::STORE) {
                Some(Kind::RwStorage)
            } else {
                Some(Kind::Storage)
            }
        }
        AddressSpace::Handle => match &module.types[var.ty].inner {
            TypeInner::Sampler { .. } => Some(Kind::Sampler),
            TypeInner::Image { class, .. } => match class {
                ImageClass::Storage { access, .. } => {
                    if access.contains(StorageAccess::STORE) {
                        Some(Kind::RwStorageTexture)
                    } else {
                        Some(Kind::StorageTexture)
                    }
                }
                _ => Some(Kind::Texture),
            },
            _ => None,
        },
        _ => None,
    }
}

/// (image, sampler) global pairs used by sampling expressions anywhere in the module.
fn sampler_pairs(module: &naga::Module) -> Vec<(Handle<naga::GlobalVariable>, Handle<naga::GlobalVariable>)> {
    let mut pairs = Vec::new();
    let mut scan = |f: &naga::Function| {
        for (_, expr) in f.expressions.iter() {
            if let Expression::ImageSample { image, sampler, .. } = expr {
                if let (Expression::GlobalVariable(i), Expression::GlobalVariable(s)) =
                    (&f.expressions[*image], &f.expressions[*sampler])
                {
                    pairs.push((*i, *s));
                }
            }
        }
    };
    for (_, f) in module.functions.iter() {
        scan(f);
    }
    for ep in module.entry_points.iter() {
        scan(&ep.function);
    }
    pairs
}

fn translate(src: &str, stage: ShaderStage, entry: &str) -> Result<String, String> {
    let module = naga::front::wgsl::parse_str(src).map_err(|e| e.emit_to_string(src))?;
    let info = Validator::new(ValidationFlags::all(), Capabilities::all())
        .validate(&module)
        .map_err(|e| format!("{e:?}"))?;
    let ep_index = module
        .entry_points
        .iter()
        .position(|ep| ep.stage == stage && ep.name == entry)
        .ok_or_else(|| format!("no {stage:?} entry point {entry}"))?;
    let ep_info = info.get_entry_point(ep_index);

    // The resources this entry point uses, by (group, binding).
    let mut used: BTreeMap<(u32, u32), (Handle<naga::GlobalVariable>, Kind)> = BTreeMap::new();
    for (handle, var) in module.global_variables.iter() {
        let (Some(binding), Some(kind)) = (&var.binding, classify(&module, var)) else { continue };
        if ep_info[handle].is_empty() {
            continue;
        }
        used.insert((binding.group, binding.binding), (handle, kind));
    }

    // Slots per SDL_GPU's MSL rules: textures in their own space (sampled first), buffers uniform-first.
    let mut slot: BTreeMap<(u32, u32), u32> = BTreeMap::new();
    let mut next_texture = 0u32;
    for kind in [Kind::Texture, Kind::StorageTexture, Kind::RwStorageTexture] {
        for (key, (_, k)) in &used {
            if *k == kind {
                slot.insert(*key, next_texture);
                next_texture += 1;
            }
        }
    }
    let mut next_buffer = 0u32;
    for kind in [Kind::Uniform, Kind::Storage, Kind::RwStorage] {
        for (key, (_, k)) in &used {
            if *k == kind {
                slot.insert(*key, next_buffer);
                next_buffer += 1;
            }
        }
    }
    // A sampler takes the slot of the first sampled texture it is used with.
    let pairs = sampler_pairs(&module);
    let texture_slot_of = |g: Handle<naga::GlobalVariable>| -> Option<u32> {
        let b = module.global_variables[g].binding.as_ref()?;
        slot.get(&(b.group, b.binding)).copied()
    };
    let mut sampler_slot: BTreeMap<(u32, u32), u32> = BTreeMap::new();
    for (key, (handle, kind)) in &used {
        if *kind != Kind::Sampler {
            continue;
        }
        let paired = pairs.iter().filter(|(_, s)| s == handle).find_map(|(i, _)| texture_slot_of(*i));
        sampler_slot.insert(*key, paired.ok_or_else(|| format!("sampler {key:?} is not paired with a texture"))?);
    }

    let count = |kinds: &[Kind]| used.values().filter(|(_, k)| kinds.contains(k)).count();
    let mut resources = BTreeMap::new();
    for (key, (_, kind)) in &used {
        let rb = ResourceBinding { group: key.0, binding: key.1 };
        let mut target = msl::BindTarget::default();
        match kind {
            Kind::Uniform | Kind::Storage | Kind::RwStorage => target.buffer = Some(slot[key] as u8),
            Kind::Texture | Kind::StorageTexture | Kind::RwStorageTexture => {
                target.texture = Some(slot[key] as u8);
                target.mutable = *kind == Kind::RwStorageTexture;
            }
            Kind::Sampler => target.sampler = Some(msl::BindSamplerTarget::Resource(sampler_slot[key] as u8)),
        }
        resources.insert(rb, target);
    }

    // naga's MSL passes runtime-sized arrays' byte sizes in a hidden buffer. It takes the next buffer slot
    // after the storage buffers and is bound like a read-only storage buffer holding one u32 per array.
    let runtime_sized: Vec<(u32, u32)> = used
        .iter()
        .filter(|(_, (h, k))| {
            matches!(k, Kind::Storage | Kind::RwStorage)
                && module.types[module.global_variables[*h].ty].inner.needs_host_buffer_byte_size(&module.types)
        })
        .map(|(key, _)| *key)
        .collect();
    let sizes_slot = (!runtime_sized.is_empty()).then_some(next_buffer);

    let mut options = msl::Options::default();
    options.lang_version = (2, 4);
    options.fake_missing_bindings = false;
    options.zero_initialize_workgroup_memory = true;
    // aurora runs Dawn with robustness disabled; unchecked access also needs no hidden array-sizes buffer,
    // which SDL_GPU has no slot for.
    options.bounds_check_policies = naga::proc::BoundsCheckPolicies {
        index: naga::proc::BoundsCheckPolicy::Unchecked,
        buffer: naga::proc::BoundsCheckPolicy::Unchecked,
        image_load: naga::proc::BoundsCheckPolicy::Unchecked,
        binding_array: naga::proc::BoundsCheckPolicy::Unchecked,
    };
    options.per_entry_point_map.insert(
        entry.to_string(),
        msl::EntryPointResources { resources, sizes_buffer: sizes_slot.map(|s| s as u8), ..Default::default() },
    );
    let pipeline_options = msl::PipelineOptions {
        entry_point: Some((stage, entry.to_string())),
        ..Default::default()
    };
    let (source, translation) =
        msl::write_string(&module, &info, &options, &pipeline_options).map_err(|e| format!("{e:?}"))?;
    // With PipelineOptions::entry_point set, naga writes (and names) only that entry point.
    let entry_name = translation
        .entry_point_names
        .first()
        .and_then(|name| name.as_ref().ok())
        .cloned()
        .ok_or_else(|| {
            format!(
                "naga produced no MSL name for {entry} (index {ep_index}, names {:?})",
                translation.entry_point_names
            )
        })?;

    let mut out = String::new();
    let _ = writeln!(out, "entry {entry_name}");
    let _ = writeln!(
        out,
        "counts {} {} {} {} {} {}",
        count(&[Kind::Texture]),
        count(&[Kind::StorageTexture]),
        // The sizes buffer takes the next [[buffer]] index: after the read-only storage buffers in graphics
        // stages, after the read-write ones in compute, so it counts with that group.
        count(&[Kind::Storage]) + (sizes_slot.is_some() && stage != ShaderStage::Compute) as usize,
        count(&[Kind::Uniform]),
        count(&[Kind::RwStorageTexture]),
        count(&[Kind::RwStorage]) + (sizes_slot.is_some() && stage == ShaderStage::Compute) as usize,
    );
    if stage == ShaderStage::Compute {
        let [x, y, z] = module.entry_points[ep_index].workgroup_size;
        let _ = writeln!(out, "workgroup {x} {y} {z}");
    }
    let mut layouter = naga::proc::Layouter::default();
    layouter.update(module.to_ctx()).map_err(|e| format!("{e:?}"))?;
    let mut seen = BTreeSet::new();
    for (key, (handle, kind)) in &used {
        let s = if *kind == Kind::Sampler { sampler_slot[key] } else { slot[key] };
        if seen.insert(*key) {
            // A uniform's line also carries the bytes the shader reads, the size to push per draw.
            let size = if *kind == Kind::Uniform { layouter[module.global_variables[*handle].ty].size } else { 0 };
            let _ = writeln!(out, "bind {} {} {} {} {}", key.0, key.1, kind.name(), s, size);
        }
    }
    if let Some(s) = sizes_slot {
        // sizes <slot> <group binding>...: the u32 byte sizes, in this order, of the arrays listed.
        let _ = write!(out, "sizes {s}");
        for (g, b) in &runtime_sized {
            let _ = write!(out, " {g} {b}");
        }
        out.push('\n');
    }
    out.push_str("---\n");
    out.push_str(&source);
    Ok(out)
}

/// stage: 0 vertex, 1 fragment, 2 compute. On success returns 0 and a malloc'd NUL-terminated blob in *out;
/// on failure returns 1 and the error message in *out. The caller frees *out with free().
#[no_mangle]
pub unsafe extern "C" fn aurora_wgsl_to_msl(
    src: *const c_char,
    stage: u32,
    entry: *const c_char,
    out: *mut *mut c_char,
) -> i32 {
    let src = CStr::from_ptr(src).to_string_lossy();
    let entry = CStr::from_ptr(entry).to_string_lossy();
    let stage = match stage {
        0 => ShaderStage::Vertex,
        1 => ShaderStage::Fragment,
        _ => ShaderStage::Compute,
    };
    let (code, text) = match translate(&src, stage, &entry) {
        Ok(text) => (0, text),
        Err(e) => (1, e),
    };
    let bytes = text.into_bytes();
    let buf = libc_malloc(bytes.len() + 1) as *mut u8;
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf, bytes.len());
    *buf.add(bytes.len()) = 0;
    *out = buf as *mut c_char;
    code
}

extern "C" {
    #[link_name = "malloc"]
    fn libc_malloc(size: usize) -> *mut std::ffi::c_void;
}

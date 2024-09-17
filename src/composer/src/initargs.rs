use passes::{component, BuildState, ComponentId, InitParamPass, SystemState, TransitionIter};
use syshelpers::emit_file;

#[derive(Debug, Clone)]
pub enum ArgsValType {
    Str(String),
    Arr(Vec<ArgsKV>),
}

#[derive(Debug, Clone)]
pub struct ArgsKV {
    key: String,
    val: ArgsValType,
}

struct VarNamespace {
    id: u32,
}

impl VarNamespace {
    fn new() -> VarNamespace {
        VarNamespace { id: 0 }
    }

    fn fresh_name(&mut self) -> String {
        let id = self.id;
        self.id = self.id + 1;
        format!("__initargs_autogen_{}", id)
    }
}

impl ArgsKV {
    pub fn new_key(key: String, val: String) -> ArgsKV {
        ArgsKV {
            key,
            val: ArgsValType::Str(val),
        }
    }

    pub fn new_arr(key: String, val: Vec<ArgsKV>) -> ArgsKV {
        ArgsKV {
            key,
            val: ArgsValType::Arr(val),
        }
    }

    pub fn new_top(val: Vec<ArgsKV>) -> ArgsKV {
        ArgsKV {
            key: String::from("_"),
            val: ArgsValType::Arr(val),
        }
    }

    // This provides code generation for the data-structure containing
    // the initial arguments for the component.  Return a string
    // accumulating new definitions, and another accumulating arrays.
    fn serialize_rec(&self, ns: &mut VarNamespace) -> (String, Vec<String>) {
        match &self {
            ArgsKV {
                key: k,
                val: ArgsValType::Str(ref s),
            } => {
                // base case
                let kv_name = ns.fresh_name();
                (
                    format!(
                        r#"static struct kv_entry {} = {{ key: "{}", vtype: VTYPE_STR, val: {{ str: "{}" }} }};
"#,
                        kv_name, k, s
                    ),
                    vec![format!("&{}", kv_name)],
                )
            }
            ArgsKV {
                key: k,
                val: ArgsValType::Arr(ref kvs),
            } => {
                let arr_val_name = ns.fresh_name(); // the array value structure
                let arr_name = ns.fresh_name(); // the actual array
                                                // recursive call to serialize all nested K/Vs
                let strs = kvs
                    .iter()
                    .fold((String::from(""), Vec::new()), |(t, s), kv| {
                        let (t1, s1) = kv.serialize_rec(ns);

                        let mut exprs = Vec::new();
                        exprs.extend(s1);
                        exprs.extend(s);

                        (format!("{}{}", t, t1), exprs)
                    });
                (
                    format!(
                        r#"{}static struct kv_entry *{}[] = {{{}}};
static struct kv_entry {} = {{ key: "{}", vtype: VTYPE_ARR, val: {{ arr: {{ sz: {}, kvs: {} }} }} }};
"#,
                        strs.0,
                        arr_name,
                        strs.1.join(", "),
                        arr_val_name,
                        k,
                        kvs.len(),
                        arr_name
                    ),
                    vec![format!("&{}", arr_val_name)],
                )
            }
        }
    }

    // Generate the c data-structure for the initial arguments to be
    // paired with the cosargs library
    pub fn serialize(&self) -> String {
        let mut ns = VarNamespace::new();

        format!("#include <initargs.h>
{}
struct initargs __initargs_root = {{ type: ARGS_IMPL_KV, d: {{ kv_ent: &__initargs_autogen_0 }} }};", self.serialize_rec(&mut ns).0)
    }
}

// The key within the initargs for the tarball, the path of the
// tarball, and the set of paths to the files to include in the
// tarball and name of them within the tarball.
// use tar::Builder;
// fn tarball_create(
//     tarball_key: &String,
//     tar_path: &String,
//     contents: Vec<(String, String)>,
// ) -> Result<(), String> {
//     let file = File::create(&tar_path).unwrap();
//     let mut ar = Builder::new(file);
//     let key = format!("{}/", tarball_key);

//     ar.append_dir(&key, &key).unwrap(); // FIXME: error handling
//     contents.iter().for_each(|(p, n)| {
//         // file path, and name for the tarball
//         let mut f = File::open(p).unwrap(); //  should not fail: we just built this, TODO: fix race
//         ar.append_file(format!("{}/{}", tarball_key, n), &mut f)
//             .unwrap(); // FIXME: error handling
//     });
//     ar.finish().unwrap(); // FIXME: error handling
//     Ok(())
// }

fn initargs_create(initargs_path: &String, kvs: &Vec<ArgsKV>) -> Result<(), String> {
    let top = ArgsKV::new_top(kvs.clone());
    let args = top.serialize();

    if let Err(s) = emit_file(&initargs_path, args.as_bytes()) {
        return Err(s);
    }

    Ok(())
}

// This is per-component.
pub struct Parameters {
    param_file_path: String,
    tar_file_path: Option<String>,
    args: Vec<ArgsKV>,
}

impl InitParamPass for Parameters {
    fn param_prog(&self) -> &String {
        &self.param_file_path
    }

    fn param_list(&self) -> &Vec<ArgsKV> {
        &self.args
    }

    fn param_fs(&self) -> &Option<String> {
        &self.tar_file_path
    }
}

impl TransitionIter for Parameters {
    fn transition_iter(
        id: &ComponentId,
        s: &SystemState,
        b: &mut dyn BuildState,
        _stack_size: Option<&String>,
    ) -> Result<Box<Self>, String> {
        let argpath = b.comp_file_path(&id, &"initargs.c".to_string(), s)?;
        let mut args = Vec::new();

        let param_args = component(s, id).params.clone();
        args.push(ArgsKV::new_arr(String::from("param"), param_args));
        let resargs = s.get_restbl().args(&id);
        resargs.iter().for_each(|a| args.push(a.clone()));
        args.push(ArgsKV::new_key(String::from("compid"), id.to_string()));

        initargs_create(&argpath, &args)?;

        Ok(Box::new(Parameters {
            args: args.clone(),
            param_file_path: argpath,
            tar_file_path: None,
        }))
    }
}

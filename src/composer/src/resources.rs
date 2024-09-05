use initargs::ArgsKV;
use passes::{
    component, BuildState, ComponentId, ComponentName, OrderedSpecPass, VirtResPass, PropertiesPass, ResPass, ServiceType,
    SystemState, Transition,
};
use std::collections::{BTreeMap, HashMap};

// Resource must be allocated and delegated to populate the resource
// tables for each component, and determine which is in charge of
// which resources.
#[derive(Debug)]
enum CapRes {
    CapTbl(ComponentId),
    PgTbl(ComponentId),
    Comp(ComponentId),
}

const BOOT_CAPTBL_FREE: u32 = 88;

// The equivalent of the C __captbl_cap2sz(c)
fn cap_sz(cap: &CapRes) -> u32 {
    match cap {
        CapRes::CapTbl(_) | CapRes::PgTbl(_) => 4,
        CapRes::Comp(_) => 4,
    }
}

// returns the size of the capability, the type (in captbl, pgtbl,
// comp, for now), the target, and the indirect target.
fn cap_info(c: &CapRes) -> (String, ComponentId) {
    let (n, id) = match c {
        CapRes::CapTbl(id) => ("captbl".to_string(), id),
        CapRes::PgTbl(id) => ("pgtbl".to_string(), id),
        CapRes::Comp(id) => ("comp".to_string(), id),
    };

    (n, *id)
}

struct CaptblState {
    captbl: BTreeMap<u32, CapRes>,
    frontier: u32,
    prev_sz: u32,
}

impl CaptblState {
    fn new() -> CaptblState {
        CaptblState {
            captbl: BTreeMap::new(),
            frontier: BOOT_CAPTBL_FREE, // BOOT_CAPTBL_FREE...FIXME: currently use shell script automatically generate this, is there a more elegant way?
            prev_sz: 4,
        }
    }

    fn add(&mut self, cap: CapRes) {
        let sz = cap_sz(&cap);
        let frontier = self.frontier;

        if (frontier % 4 != 0) && (self.prev_sz != sz) {
            self.frontier = frontier + (4 - frontier % 4); //  round up to power of 4
        }
        self.frontier += sz;
        self.prev_sz = sz;

        self.captbl.insert(frontier, cap);
    }

    fn get_captbl(&self) -> &BTreeMap<u32, CapRes> {
        &self.captbl
    }

    fn get_frontier(&self) -> u32 {
        self.frontier.clone()
    }
}

struct CompConfigState {
    ct: CaptblState,
    args: Vec<ArgsKV>,
}

impl CompConfigState {
    fn new() -> CompConfigState {
        CompConfigState {
            ct: CaptblState::new(),
            args: Vec::new(),
        }
    }
}

fn vr_server_config(s: &SystemState, id: &ComponentId, cfg: &mut CompConfigState) {
    let mut virtual_resources_args = Vec::new();
    let vr_orig = s.get_spec().virtual_resources();
    let vr_pass: &dyn VirtResPass = s.get_virt_res();
    let vas: &dyn OrderedSpecPass = s.get_named();
    let component_name = vas.ids().get(id).unwrap();

    for (_key, vr) in vr_orig {
        if vr.server == component_name.var_name {
            if let Some(vrs) = vr_pass.virt_res_with_id().get(&vr.name) {
                let mut vr_args = Vec::new();

                for shmem in &vrs.resources {
                        let mut clients_args = Vec::new();
                        for client in &shmem.clients {
                            let client_id = vas.rmap().get(&ComponentName::new(&client.comp, &String::from("global"))).unwrap().to_string();
                            clients_args.push(ArgsKV::new_arr(
                                "client".to_string(),
                                vec![
                                    ArgsKV::new_key("compid".to_string(), client_id),
                                    ArgsKV::new_key("access".to_string(), client.access.clone()),
                                ],
                            ));
                        }

                        let mut param_args = Vec::new();
                        for (key, value) in &shmem.param {
                            param_args.push(ArgsKV::new_key(key.clone(), value.clone()));
                        }
                        
                        vr_args.push(ArgsKV::new_arr(
                            "sub_virt_resource".to_string(),
                            vec![
                                ArgsKV::new_key("id".to_string(), shmem.virt_resource_id.clone()),
                                ArgsKV::new_arr("params".to_string(), param_args),
                                ArgsKV::new_arr("clients".to_string(), clients_args),
                            ],
                        ));
                }
                virtual_resources_args.push(ArgsKV::new_arr(vr.name.clone(), vr_args));
            }
        }
    }
    
    if !virtual_resources_args.is_empty() {
        cfg.args
        .push(ArgsKV::new_arr("virt_resources".to_string(), virtual_resources_args));
    }
}

fn vr_client_config(_s: &SystemState, _id: &ComponentId, _cfg: &mut CompConfigState) {
    let vas: &dyn OrderedSpecPass = _s.get_named();
    let vr_pass: &dyn VirtResPass = _s.get_virt_res();

    let mut virtual_resources_args = Vec::new();
    let vr_orig = _s.get_spec().virtual_resources();

    for (_key, vr) in vr_orig {
        if let Some(vrs) = vr_pass.virt_res_with_id().get(&vr.name) {
            let mut vr_args = Vec::new();

            for virtres in &vrs.resources {
                for client in &virtres.clients {
                    let client_id = vas.rmap().get(&ComponentName::new(&client.comp, &String::from("global"))).unwrap().to_string();

                    if client_id == _id.to_string() {
                        let mut param_args = Vec::new();
                        for (key, value) in &virtres.param {
                            param_args.push(ArgsKV::new_key(key.clone(), value.clone()));
                        }

                        vr_args.push(ArgsKV::new_arr(
                            "sub_virt_resource".to_string(),
                            vec![
                                ArgsKV::new_key("id".to_string(), virtres.virt_resource_id.clone()),
                                ArgsKV::new_arr("params".to_string(), param_args),
                                ArgsKV::new_key("name".to_string(), client.name.clone().unwrap_or_default()),
                                ArgsKV::new_key("symbol".to_string(), client.symbol.clone().unwrap_or_default()),
                                ArgsKV::new_key("access".to_string(), client.access.clone()),
                            ],
                        ));
                    }
                }
            }

            if !vr_args.is_empty() {
                virtual_resources_args.push(ArgsKV::new_arr(
                    vr.name.clone(),vr_args));
            }
        }
    }
    
    if !virtual_resources_args.is_empty() {
        _cfg.args.push(ArgsKV::new_arr(
            "virt_resources".to_string(),
            virtual_resources_args,
        ));
    }
}

fn comp_config(_s: &SystemState, _id: &ComponentId, _cfg: &mut CompConfigState) {
    vr_server_config(_s, _id, _cfg);
    vr_client_config(_s, _id, _cfg);
}

fn sched_config_serv_client(s: &SystemState, id: &ComponentId) -> Vec<ArgsKV> {
    let mut init = Vec::new();

    let props: &dyn PropertiesPass = s.get_properties();
    if !props.service_is_a(&id, ServiceType::Scheduler) {
        return init;
    }

    let clients = props.service_clients(&id, ServiceType::Scheduler);

    assert!(props.service_is_a(&id, ServiceType::Scheduler));
    if let Some(cs) = clients {
        for c in cs.iter() {
            init.push(ArgsKV::new_key(c.to_string(), id.to_string()));
        }
    }

    init
}

fn sched_config_clients(s: &SystemState, id: &ComponentId) -> Vec<ArgsKV> {
    let mut init = Vec::new();

    let props: &dyn PropertiesPass = s.get_properties();
    if !props.service_is_a(&id, ServiceType::Scheduler) {
        return init;
    }

    let clients = props.service_clients(&id, ServiceType::Scheduler);

    assert!(props.service_is_a(&id, ServiceType::Scheduler));
    if let Some(cs) = clients {
        for c in cs.iter() {
            init.push(
                // key: component id and value: style of initialization
                ArgsKV::new_key(
                    c.to_string(),
                    if props.service_is_a(c, ServiceType::Scheduler) {
                        "sched".to_string()
                    } else {
                        "init".to_string()
                    },
                ),
            );
        }
    }

    // The pushes reversed the order
    init.reverse();
    init
}

fn sched_config(s: &SystemState, id: &ComponentId, cfg: &mut CompConfigState) {
    cfg.args.push(ArgsKV::new_arr(
        "execute".to_string(),
        sched_config_clients(&s, &id),
    ));
}

fn cap2kvarg(capid: u32, cap: &CapRes) -> ArgsKV {
    let (name, target) = cap_info(&cap);
    let capinfo = vec![
        ArgsKV::new_key("type".to_string(), name),
        ArgsKV::new_key("target".to_string(), target.to_string()),
    ];
    ArgsKV::new_arr(capid.to_string(), capinfo)
}

fn capmgr_config(s: &SystemState, id: &ComponentId, cfg: &mut CompConfigState) {
    let props: &dyn PropertiesPass = s.get_properties();
    if !props.service_is_a(&id, ServiceType::CapMgr) {
        return;
    }

    let mut clients = props
        .service_clients(&id, ServiceType::Scheduler)
        .map(|cs| cs.clone()) // get rid of the reference
        .unwrap_or_else(|| Vec::new());
    let mut sched_args = Vec::new();
    let mut ct_args = Vec::new();
    let mut init_args = Vec::new();
    let mut names_args = Vec::new();

    // aggregate records for scheduler and capmgr dependencies
    clients.append(
        &mut props
            .service_clients(&id, ServiceType::CapMgr)
            .map(|cs| cs.clone())
            .unwrap_or_else(|| Vec::new()),
    );
    clients.sort();
    clients.dedup();

    assert!(props.service_is_a(&id, ServiceType::CapMgr));
    for c in &clients {
        // sanity
        assert!(*c != *id);
        // don't support nested capmgrs yet
        assert!(!props.service_is_a(&c, ServiceType::CapMgr));

        // capability table entries for the client
        cfg.ct.add(CapRes::CapTbl(*c));
        cfg.ct.add(CapRes::PgTbl(*c));
        cfg.ct.add(CapRes::Comp(*c));

        // scheduler hierarchy
        if props.service_is_a(&c, ServiceType::Scheduler) {
            let p = props.service_dependency(&c, ServiceType::Scheduler);

            // at the least, the capmgr should be the parent
            assert!(p.is_some());
            sched_args.push(ArgsKV::new_key(c.to_string(), p.unwrap().to_string()));
        }

        // Initialization information for all components shipped to
        // the capmgr This effectively grants permission for the
        // scheduler to create execution in a client.
        let mut init_clients = sched_config_serv_client(&s, &c);
        if !init_clients.is_empty() {
            init_args.append(&mut init_clients);
        }

        // client names
        let spec_comp = component(&s, &c);
        let name = format!(
            "{}.{}.{}",
            spec_comp.source, spec_comp.name.scope_name, spec_comp.name.var_name
        );
        names_args.push(ArgsKV::new_key(c.to_string(), name));
    }

    // Lets provide information to the capability manager about which
    // components are in shared, and which are in exclusive address
    // spaces.
    let mut shared_vas = Vec::new();
    let vas: &dyn OrderedSpecPass = s.get_named();
    for (_, addrspc) in vas.addrspc_components_shared() {
        for c in &addrspc.components {
            // unwrap as every name should be represented (see OrderedSpecpass).
            let id = vas.rmap().get(&c).unwrap();
            shared_vas.push(ArgsKV::new_key("_".to_string(), format!("{}", id)));
        }
    }
         
    cfg.args.push(ArgsKV::new_arr(
        "scheduler_hierarchy".to_string(),
        sched_args,
    ));
    cfg.args
        .push(ArgsKV::new_arr("init_hierarchy".to_string(), init_args));

    for (capid, cap) in cfg.ct.get_captbl() {
        ct_args.push(cap2kvarg(*capid, &cap));
    }
    cfg.args
        .push(ArgsKV::new_arr("captbl".to_string(), ct_args));
    cfg.args
        .push(ArgsKV::new_arr("names".to_string(), names_args));
    cfg.args
        .push(ArgsKV::new_arr("addrspc_shared".to_string(), shared_vas));
}

fn constructor_config(s: &SystemState, id: &ComponentId, cfg: &mut CompConfigState) {
    let props: &dyn PropertiesPass = s.get_properties();
    if !props.service_is_a(&id, ServiceType::Constructor) {
        return;
    }

    let clients = props.service_clients(&id, ServiceType::Constructor);
    let mut captbls = BTreeMap::new();

    assert!(props.service_is_a(&id, ServiceType::Constructor));
    // for now, assume only a single constructor
    assert!(props
        .service_dependency(&id, ServiceType::Constructor)
        .is_none());
    if let Some(cs) = clients {
        for c in cs {
            // only have captbl for capmgrs
            if !props.service_is_a(&c, ServiceType::CapMgr) {
                continue;
            }

            // Assuming that the values returned by this are the
            // same as those created while processing the capmgr
            let mut chld_cfg = CompConfigState::new();
            capmgr_config(&s, &c, &mut chld_cfg);
            captbls.insert(*c, chld_cfg);
        }
    }

    // OK, then we should construct the arguments
    let mut args = Vec::new();
    for (id, cfg) in &captbls {
        let mut comp_args = Vec::new();
        let ct = cfg.ct.get_captbl();

        for (capid, cap) in ct {
            comp_args.push(cap2kvarg(*capid, &cap));
        }

        args.push(ArgsKV::new_arr(id.to_string(), comp_args));
    }

    cfg.args
        .push(ArgsKV::new_arr("captbl_delegations".to_string(), args));
    // FIXME: move some of the build.rs logic for constructor creation here.
}

fn comp_config_finalize(_s: &SystemState, _id: &ComponentId, cfg: CompConfigState) -> Vec<ArgsKV> {
    let frontier = cfg.ct.get_frontier();
    let mut cfg_mut = cfg;

    // TODO: Note that this is the end *before* all of the sinv allocations
    cfg_mut.args.push(ArgsKV::new_key(
        "captbl_end".to_string(),
        frontier.to_string(),
    ));
    // this clone is the cost of the type making it clear this is the
    // "sink" for cfg
    cfg_mut.args.clone()
}

pub struct ResAssignPass {
    resources: HashMap<ComponentId, Vec<ArgsKV>>,
}

impl Transition for ResAssignPass {
    fn transition(s: &SystemState, _b: &mut dyn BuildState) -> Result<Box<Self>, String> {
        let mut res = HashMap::new();

        for (k, _v) in s.get_named().ids().iter() {
            let mut cfg = CompConfigState::new();

            // capmgr configuration must be first, and before
            // constructor configuration, as constructor has to be
            // able to replicate each capmgr's captbl layout
            capmgr_config(&s, &k, &mut cfg);
            constructor_config(&s, &k, &mut cfg);
            sched_config(&s, &k, &mut cfg);
            comp_config(&s, &k, &mut cfg);
            res.insert(k.clone(), comp_config_finalize(&s, &k, cfg));
        }

        Ok(Box::new(ResAssignPass { resources: res }))
    }
}

impl ResPass for ResAssignPass {
    fn args(&self, id: &ComponentId) -> &Vec<ArgsKV> {
        &self.resources.get(&id).unwrap()
    }
}

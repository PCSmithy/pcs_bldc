//! Board wiring: the routes binding a firmware instance's observation ports to the
//! models that consume them.

use voyant::{vsig_id, Engine, SignalId};

/// The firmware bridge port paired with the motor input it drives, in wiring order.
const BRIDGE_PORTS: [(&str, &str); 7] = [
    ("PWM_U_duty", "duty_u"),
    ("PWM_V_duty", "duty_v"),
    ("PWM_W_duty", "duty_w"),
    ("PWM_U_enabled", "enable_u"),
    ("PWM_V_enabled", "enable_v"),
    ("PWM_W_enabled", "enable_w"),
    ("TIM1_MOE", "moe"),
];

/// The seven `(src, dst)` endpoint pairs [`wire_bridge`] installed, in [`BRIDGE_PORTS`]
/// order. Each pair addresses one route for [`Engine::suspend_route`] /
/// [`Engine::resume_route`], the fault-injection seam.
pub struct BridgeRoutes {
    routes: [(SignalId, SignalId); 7],
}

impl BridgeRoutes {
    /// The route endpoints, for suspending or resuming an individual edge.
    pub fn routes(&self) -> &[(SignalId, SignalId)] {
        &self.routes
    }

    /// Suspend every bridge route, leaving the motor's inputs to direct writes.
    pub fn suspend_all(&self, eng: &mut Engine) -> Result<(), voyant::EngineError> {
        for (src, dst) in &self.routes {
            eng.suspend_route(src, dst)?;
        }
        Ok(())
    }

    /// Resume every bridge route, handing the motor's inputs back to the firmware.
    pub fn resume_all(&self, eng: &mut Engine) -> Result<(), voyant::EngineError> {
        for (src, dst) in &self.routes {
            eng.resume_route(src, dst)?;
        }
        Ok(())
    }
}

/// Bind firmware `fw`'s seven bridge observation ports (per-phase duty and enable plus
/// the master output enable) to motor `motor`'s command inputs. The routes are delayed
/// by one tick: the firmware's commands act on the plant the following tick.
pub fn wire_bridge(
    eng: &mut Engine,
    fw: &str,
    motor: &str,
) -> Result<BridgeRoutes, Box<dyn std::error::Error>> {
    let mut routes = Vec::with_capacity(BRIDGE_PORTS.len());
    for (port, input) in BRIDGE_PORTS {
        let src = vsig_id(fw, port)?;
        let dst = vsig_id(motor, input)?;
        eng.add_delayed_route(src.clone(), dst.clone())?;
        routes.push((src, dst));
    }
    Ok(BridgeRoutes {
        routes: routes.try_into().expect("seven bridge routes"),
    })
}

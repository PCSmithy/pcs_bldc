//! Board wiring: the routes binding a firmware instance's observation ports to the
//! models that consume them, and the models to each other.

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

/// Motor current output paired with the sense-model input it drives.
const SENSE_INPUT_PORTS: [(&str, &str); 4] = [
    ("phase_current_u", "i_u"),
    ("phase_current_v", "i_v"),
    ("phase_current_w", "i_w"),
    ("bus_current", "i_bus"),
];

/// Sense-model output paired with the firmware ADC port it drives (the board's
/// shunt-to-pin mapping from `IO_bridge_channels.c`).
const SENSE_OUTPUT_PORTS: [(&str, &str); 4] = [
    ("i_u_vsense", "ADC1_IN6"),
    ("i_v_vsense", "ADC2_IN7"),
    ("i_w_vsense", "ADC1_IN8"),
    ("i_bus_vsense", "ADC2_IN11"),
];

/// The `(src, dst)` endpoint pairs a wiring helper installed, in its port-table
/// order. Each pair addresses one route for [`Engine::suspend_route`] /
/// [`Engine::resume_route`], the fault-injection seam.
pub struct RouteBundle<const N: usize> {
    routes: [(SignalId, SignalId); N],
}

/// The seven delayed bridge routes [`wire_bridge`] installs.
pub type BridgeRoutes = RouteBundle<7>;
/// The eight zero-latency sense routes [`wire_current_sense`] installs.
pub type CurrentSenseRoutes = RouteBundle<8>;

impl<const N: usize> RouteBundle<N> {
    /// The route endpoints, for suspending or resuming an individual edge.
    pub fn routes(&self) -> &[(SignalId, SignalId)] {
        &self.routes
    }

    /// Suspend every route in the bundle, leaving the destinations to direct writes.
    pub fn suspend_all(&self, eng: &mut Engine) -> Result<(), voyant::EngineError> {
        for (src, dst) in &self.routes {
            eng.suspend_route(src, dst)?;
        }
        Ok(())
    }

    /// Resume every route in the bundle, handing the destinations back to their
    /// sources.
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
    Ok(RouteBundle {
        routes: routes.try_into().expect("seven bridge routes"),
    })
}

/// Bind motor `motor`'s current outputs to sense model `sense`'s inputs, and the
/// sense model's pin-voltage outputs to firmware `fw`'s ADC ports. All eight routes
/// are zero-latency: with members registered motor → sense → firmware, the firmware
/// samples the same tick's plant currents — no artificial sensing delay.
pub fn wire_current_sense(
    eng: &mut Engine,
    motor: &str,
    sense: &str,
    fw: &str,
) -> Result<CurrentSenseRoutes, Box<dyn std::error::Error>> {
    let mut routes = Vec::with_capacity(SENSE_INPUT_PORTS.len() + SENSE_OUTPUT_PORTS.len());
    for (output, input) in SENSE_INPUT_PORTS {
        let src = vsig_id(motor, output)?;
        let dst = vsig_id(sense, input)?;
        eng.add_route(src.clone(), dst.clone())?;
        routes.push((src, dst));
    }
    for (output, port) in SENSE_OUTPUT_PORTS {
        let src = vsig_id(sense, output)?;
        let dst = vsig_id(fw, port)?;
        eng.add_route(src.clone(), dst.clone())?;
        routes.push((src, dst));
    }
    Ok(RouteBundle {
        routes: routes.try_into().expect("eight sense routes"),
    })
}

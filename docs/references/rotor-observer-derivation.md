# Rotor observer derivation

Supplement to `fw~est_rotor_001~1` and `fw~est_rotor_002~1`
(`specs/firmware/est/rotor.md`). The design-C rotor estimator is a
third-order Luenberger observer on $[\theta,\ \omega,\ T_d]^\mathsf{T}$
with the encoder angle as its one measurement. This note derives it end
to end: the plant model, the observer and its error dynamics, the pole
placement that gives the gains, and the two discretisations the firmware
runs, prediction at the PWM rate and correction at the encoder rate.

## 1. Plant

Newton on the rotor, with the two friction terms the model names and
everything else lumped into one unknown torque $T_d$:

$$J\,\dot\omega = \tau - b\,\omega - T_c\,\operatorname{sgn}(\omega) + T_d,
\qquad \dot\theta = \omega$$

$\tau$ is the torque the motor-control application commands ($K_t\,i_q$),
$b$ the viscous friction, $T_c$ the Coulomb friction, $J$ the rotor
inertia. $T_d$ is whatever acts on the rotor that the model does not
name: a hand on the shaft, a load, a friction error.

$T_d$ has no physics of its own, so the only thing the model can say
about it is that it changes slowly compared with the observer. That is
the constant-disturbance model:

$$\dot T_d = 0$$

It makes $T_d$ a constant to be found, not a quantity to be predicted.
The observer's integrator then tracks any constant $T_d$ exactly, follows
a slow drift with a lag of about $1/p_d$, and pays one third-order
transient for a step. In Kalman terms the same model is a random walk,
with the process noise on $T_d$ setting the disturbance pole; in a
fixed-gain observer that pole, $p_d$, is chosen directly.

## 2. State-space form

The Coulomb term is nonlinear, so it cannot sit in a state matrix. It
goes with the commanded torque into a known input the observer evaluates
at its own estimate:

$$x = \begin{bmatrix} \theta \\ \omega \\ T_d \end{bmatrix}, \qquad
v = \tau - T_c\,\operatorname{sat}(\hat\omega/\omega_{db}), \qquad y = \theta_m$$

with $\operatorname{sat}(x) = \max(-1, \min(1, x))$: the observer's Coulomb
term is a ramp through zero, $\pm T_c$ beyond $\pm\omega_{db}$ and linear
inside, so the input stays continuous where $\hat\omega$ is noise about
zero rather than toggling by $2T_c$ with its sign.

The linear model is $\dot x = A x + B v$, $y = C x$ with

$$A = \begin{bmatrix} 0 & 1 & 0 \\ 0 & -b/J & 1/J \\ 0 & 0 & 0 \end{bmatrix},
\qquad B = \begin{bmatrix} 0 \\ 1/J \\ 0 \end{bmatrix},
\qquad C = \begin{bmatrix} 1 & 0 & 0 \end{bmatrix}$$

Row $i$ of $A$ lists the coefficients of the states in the expression
for $\dot x_i$. Row 1 says $\dot\theta = \omega$. Row 2 is the torque
balance, $\dot\omega = (-b\,\omega + T_d + v)/J$, with the
state-dependent terms in $A$ and the known part in $B$. Row 3 is
$\dot T_d = 0$.

The $1/J$ in position (2,3) is the entry that makes $T_d$ observable:
$T_d$ pushes $\omega$, $\omega$ pushes $\theta$, so a lasting angle error
can only be explained by $T_d$. Drop it and the third column of $A$ is
zero, and no gain can reach the third state.

Friction can be filed two ways. For the error dynamics in section 3 it
must stay in $A$, because the plant has $b\,\omega$ and the observer has
$b\,\hat\omega$ and the difference survives. For the prediction
arithmetic in section 5 it can move into the known input, which makes
$A$ nilpotent and the discretisation closed-form. Both uses are shown
below. The rule: $A$ must hold every state-dependent term when analysing
the error, and may hold whatever is convenient when computing the
prediction.

## 3. Observer and error dynamics

The Luenberger observer is a copy of the plant driven by the same known
input, plus a correction proportional to the measurement residual:

$$\dot{\hat x} = A\,\hat x + B\,v + L\,(y - C\,\hat x), \qquad
L = \begin{bmatrix} l_1 \\ l_2 \\ l_3 \end{bmatrix}$$

Subtract it from the plant with $e = x - \hat x$ and $y = C x$:

$$\dot e = A x + B v - A \hat x - B v - L C (x - \hat x) = (A - L C)\, e$$

The $B v$ terms cancel because both sides see the same input. That is
the whole reason the observer is fed the commanded torque: the
estimation error does not depend on what the motor is doing. The Coulomb
term does not cancel exactly, since the plant has $\operatorname{sgn}(\omega)$
and the observer \operatorname{sat}(\hat\omega/\omega_{db}); the leftover
$-(T_c/J)\,(\operatorname{sgn}\omega - \operatorname{sat}(\hat\omega/\omega_{db}))$ is
zero once $|\hat\omega|$ is past $\omega_{db}$ with the right sign, and inside
the band a torque of at most $T_c$ that the observer sees as a disturbance
and $\hat T_d$ absorbs.

The error is governed by $M = A - L C$. $L C$ is an outer product whose
only non-zero column is the first, because $C$ picks out $\theta$:

$$A - L C = \begin{bmatrix} -l_1 & 1 & 0 \\ -l_2 & -b/J & 1/J \\ -l_3 & 0 & 0 \end{bmatrix}$$

The gains sit down the first column, one per state, each in the row of
the state it corrects.

For a linear system $\dot e = M e$ the trial solution $e = e_0 e^{st}$
gives $(sI - M)\,e_0 = 0$, which has a non-zero solution only where the
matrix is singular:

$$\det(sI - M) = \det(sI - A + L C) = 0$$

The roots $s$ are the eigenvalues of $M$, and $e(t)$ is a sum of $e^{st}$
terms, so they are the rates at which the estimation error decays. The
same statement in the Laplace domain is $(sI - M)\,E(s) = e(0)$, whose
inverse puts $\det(sI - M)$ in every denominator. The roots of that
determinant are the observer's poles, and choosing $L$ is choosing where
they sit.

## 4. Pole placement

Write out the matrix whose determinant is the characteristic polynomial:

$$sI - A + L C = \begin{bmatrix} s + l_1 & -1 & 0 \\ l_2 & s + b/J & -1/J \\ l_3 & 0 & s \end{bmatrix}$$

Expand along the first row. The (1,1) cofactor is $(s + b/J)\,s$, the
(1,2) cofactor is $-(l_2 s + l_3/J)$ with the row's $-1$ in front of it,
and the (1,3) entry is zero:

$$\det = (s + l_1)(s + b/J)\,s + l_2\,s + l_3/J
= s^3 + \left(l_1 + \tfrac{b}{J}\right) s^2 + \left(l_1 \tfrac{b}{J} + l_2\right) s + \tfrac{l_3}{J}$$

Each coefficient is linear in one gain, $l_1$ in $s^2$, $l_2$ in $s$,
$l_3$ in the constant, so any cubic is reachable. That is observability
in disguise: without the $1/J$ link from $T_d$ to $\omega$, $l_3$ would
not appear and the constant coefficient would be stuck.

A polynomial is fixed by its roots, so choose where the error poles
should be and write down the polynomial that has them: a complex pair
from the standard second-order form, natural frequency $\omega_n$ and
damping $\zeta$, plus one real pole at $-p_d$ for the disturbance state.

$$(s^2 + 2\zeta\omega_n s + \omega_n^2)(s + p_d)
= s^3 + (2\zeta\omega_n + p_d)\, s^2 + (\omega_n^2 + 2\zeta\omega_n p_d)\, s + \omega_n^2 p_d$$

This desired polynomial says nothing about the plant. It is purely how
fast the error should die. At $\zeta = 1$ the pair is a double real root
at $-\omega_n$.

Two monic cubics are equal when their coefficients agree term by term.
Equate them, one equation per power of $s$:

$$\begin{aligned}
s^2:\quad & l_1 + b/J = 2\zeta\omega_n + p_d \\
s^1:\quad & l_1\, b/J + l_2 = \omega_n^2 + 2\zeta\omega_n p_d \\
s^0:\quad & l_3 / J = \omega_n^2 p_d
\end{aligned}$$

Each equation introduces exactly one new unknown, so solve top down:

$$l_1 = 2\zeta\omega_n + p_d - \frac{b}{J}, \qquad
l_2 = \omega_n^2 + 2\zeta\omega_n p_d - l_1 \frac{b}{J}, \qquad
l_3 = J\,\omega_n^2\, p_d$$

These are the gains in `fw~est_rotor_001~1`. The $J$ on $l_3$ is what
makes $\hat T_d$ a torque in newton-metres; without it the third state
would be an acceleration. The $b/J$ terms are the friction correction:
on this board $b/J$ is 4.5 rad/s against $2\zeta\omega_n + p_d$ of
785 rad/s, so leaving them out would put the poles 0.6 % off, and they
cost two terms at init.

Check against the two-state observer that preceded design C. Drop $T_d$
and $p_d$: $A$ is 2×2, the desired polynomial is
$s^2 + 2\zeta\omega_n s + \omega_n^2$, the determinant is
$s^2 + (l_1 + b/J)\,s + (l_1 b/J + l_2)$, and matching gives
$l_1 = 2\zeta\omega_n - b/J$, $l_2 = \omega_n^2 - l_1 b/J$. Every
design-C gain reduces to its design-B value as $p_d \to 0$.

## 5. Prediction step at the PWM rate

The firmware runs the observer's two halves on two clocks. The
prediction, $A\hat x + Bv$, advances the state every PWM period
$T = 50\ \mu s$ in the bridge callback. The correction runs at the 1 ms
encoder tick (section 6).

For one state with the input held over a step, $\dot x = a x + b v$ has
the solution

$$x(t+T) = e^{aT}\, x(t) + \left(\int_0^T e^{a\sigma}\, d\sigma\right) b\, v$$

The first term is the state evolving on its own, the second the input
accumulated over the step, each slice decayed by the time remaining after
it. With $a = 0$ it reduces to $x + T b v$, plain integration. The matrix
case has the identical form:

$$x(t+T) = e^{AT} x(t) + \left(\int_0^T e^{A\sigma}\, d\sigma\right) B\, v
= F\, x(t) + G\, v$$

with the matrix exponential and its integral defined by the same power
series as the scalar ones:

$$e^{AT} = I + A T + \tfrac{1}{2!} A^2 T^2 + \tfrac{1}{3!} A^3 T^3 + \cdots,
\qquad
\int_0^T e^{A\sigma}\, d\sigma = I T + \tfrac{1}{2!} A T^2 + \tfrac{1}{3!} A^2 T^3 + \cdots$$

In general the series does not terminate. For this plant it does, once
friction is moved into the known input,
$v = \tau - b\,\hat\omega - T_c\operatorname{sat}(\hat\omega/\omega_{db})$, so that
$A$ is pure kinematics plus the unknown torque:

$$A = \begin{bmatrix} 0 & 1 & 0 \\ 0 & 0 & 1/J \\ 0 & 0 & 0 \end{bmatrix},
\qquad
A^2 = \begin{bmatrix} 0 & 0 & 1/J \\ 0 & 0 & 0 \\ 0 & 0 & 0 \end{bmatrix},
\qquad A^3 = 0$$

Row 1 of $A^2$ is row 1 of $A$ applied to $A$, which picks out row 2 of
$A$; row 2 picks out $(1/J)$ times row 3, which is zero. Physically, $A$
maps $\omega$ into $\dot\theta$ and $T_d$ into $\dot\omega$, $A^2$ chains
them into $T_d$ into $\ddot\theta$, and there is no third link. A matrix
like this is nilpotent, the signature of a chain of integrators, and the
series stops at $A^2$.

$$F = I + A T + \tfrac{1}{2} A^2 T^2
= \begin{bmatrix} 1 & T & T^2/(2J) \\ 0 & 1 & T/J \\ 0 & 0 & 1 \end{bmatrix}$$

$\theta$ advances by $\omega T$ plus half $T_d T^2/J$, the
constant-acceleration kinematics; $\omega$ advances by $T_d T/J$; $T_d$
is carried unchanged, the discrete face of $\dot T_d = 0$.

$$G = \left(I T + \tfrac{1}{2} A T^2 + \tfrac{1}{6} A^2 T^3\right) B,
\qquad
I B = \begin{bmatrix} 0 \\ 1/J \\ 0 \end{bmatrix}, \quad
A B = \begin{bmatrix} 1/J \\ 0 \\ 0 \end{bmatrix}, \quad
A^2 B = 0$$

$$G = \begin{bmatrix} T^2/(2J) \\ T/J \\ 0 \end{bmatrix}$$

The known torque acts on $\theta$ and $\omega$ with the same coefficients
as $T_d$ does in $F$'s third column, as it should: both enter the plant
at the same place.

`fw~est_rotor_002~1` writes the same arithmetic in a different grouping.
It keeps a bare 1 on $F$'s $T_d$ diagonal, puts $\hat T_d/J$ inside the
input, and uses $G = [T^2/2,\ T,\ 0]^\mathsf{T}$:

$$\hat x \leftarrow \begin{bmatrix} 1 & T & 0 \\ 0 & 1 & 0 \\ 0 & 0 & 1 \end{bmatrix} \hat x
+ \begin{bmatrix} T^2/2 \\ T \\ 0 \end{bmatrix} u,
\qquad
u = \frac{\tau - b\,\hat\omega - T_c\,\operatorname{sat}(\hat\omega/\omega_{db}) + \hat T_d}{J}$$

Multiply out the $\theta$ row of both versions: exact gives
$\hat\theta + T\hat\omega + (T^2/2J)\,\hat T_d + (T^2/2J)\,v$, the spec
gives $\hat\theta + T\hat\omega + (T^2/2)(v + \hat T_d)/J$. Identical,
and likewise for $\omega$. The spec form factors $1/J$ once and treats
$\hat T_d$ as one more term of the known input, which is also what the
code does: one scalar $u$, two multiply-adds, a one-line change from the
two-state update.

The one approximation is the friction term. With $-b/J$ kept in $A$,
$F$ picks up factors of $e^{-\beta T}$ with $\beta = b/J$:
$F_{22} = e^{-\beta T}$, $F_{12} = (1 - e^{-\beta T})/\beta$, and so on.
Writing $-b\,\hat\omega\,T/J$ inside $u$ is their first-order
truncation, with a neglected term of order $(\beta T)^2/2$. At
$\beta = 4.5$ rad/s and $T = 50\ \mu s$ that is $2.5 \times 10^{-8}$
relative. The Coulomb term has to be handled this way regardless, since
the saturation cannot sit in a linear $A$.

## 6. Correction step at the encoder rate

The correction term $L\,(y - C\hat x)$ runs once per 1 ms tick $T_s$,
when a fresh encoder angle exists. Forward Euler over one tick on that
term alone:

$$\hat x \leftarrow \hat x + T_s\, L\, (y - C \hat x)$$

With $y = \theta_m$, $C\hat x = \hat\theta$, and the angle difference
wrapped to $(-\pi, \pi]$ so that a $0/2\pi$ crossing is not read as a
full turn of error:

$$e = \operatorname{wrap}(\theta_m - \hat\theta), \qquad
\hat x \leftarrow \hat x + K e, \qquad
K = T_s L = T_s \begin{bmatrix} l_1 \\ l_2 \\ l_3 \end{bmatrix}$$

In scalar form, the order `fw~est_rotor_001~1` writes it so every line
uses the same $e$:

$$\begin{aligned}
\hat T_d &\leftarrow \hat T_d + l_3\, e\, T_s \\
\hat\omega &\leftarrow \hat\omega + l_2\, e\, T_s \\
\hat\theta &\leftarrow \operatorname{wrap}(\hat\theta + l_1\, e\, T_s)
\end{aligned}$$

One angle error is spread across three states in fixed proportions
$l_1 : l_2 : l_3$. The $\hat\theta$ share closes the error directly; the
$\hat\omega$ share says that an error that keeps recurring means the
rotor is faster than estimated; the $\hat T_d$ share says that a
velocity that keeps needing correction means there is a torque the model
is missing. Each state is one integration further from the measurement
than the last, which is why the gains climb in powers of frequency.

Units: $e$ is in rad, $e\,T_s$ in rad·s, so $l_1 e T_s$ is rad,
$l_2 e T_s$ is rad/s and $l_3 e T_s$ is Nm, matching the state each line
updates.

The stability bound in the spec is a condition on the Euler step itself.
Forward Euler on a continuous pole at $-p$ gives a discrete pole at
$1 - p\,T_s$, which is stable for $p\,T_s < 2$ and non-oscillatory for
$p\,T_s \le 1$. The spec takes the determinant's $s^2$ coefficient, the
sum of the three pole magnitudes, as a conservative scalar proxy and
requires

$$(2\zeta\omega_n + p_d)\, T_s \le 1$$

which is 0.785 at the acceptance configuration. The exact discrete poles
of the full two-rate scheme differ slightly from both the Euler and the
$e^{-p T_s}$ values because the prediction and the correction are not
applied at the same instant, but with all three poles well under $1/T_s$
nothing in the acceptance depends on the difference.

What has not changed from design B: drop $p_d$ and the third state and
the correction is the two-state one with the small friction correction
on the gains. The only new line is the $\hat T_d$ update.

## 7. Numbers at the board's configuration

At the configuration the specs' acceptance bullets use,
$\omega_n = 2\pi \cdot 50$ rad/s, $\zeta = 1$, $p_d = 157$ rad/s, with
the bench-fit inertia $J = 1.6 \times 10^{-4}$ kg·m² and viscous
friction $b = 7.2 \times 10^{-4}$ Nm·s/rad ($b/J = 4.5$ rad/s). The
$b = 0$ column is what the acceptance tests see.

| Quantity | Symbol | $b = 0$ | $b = 7.2 \times 10^{-4}$ | Unit |
| --- | --- | ---: | ---: | --- |
| Error pole pair | $-\omega_n$ (double) | −314 | −314 | rad/s |
| Disturbance pole | $-p_d$ | −157 | −157 | rad/s |
| Angle gain | $l_1$ | 785 | 781 | rad/s |
| Velocity gain | $l_2$ | 197 000 | 193 800 | rad/s² |
| Torque gain | $l_3$ | 2 479 | 2 479 | Nm/rad |
| Angle correction per tick | $l_1 T_s$ | 0.785 | 0.781 | rad per rad |
| Velocity correction per tick | $l_2 T_s$ | 197 | 194 | rad/s per rad |
| Torque correction per tick | $l_3 T_s$ | 2.48 | 2.48 | Nm per rad |
| Stability proxy | $(2\zeta\omega_n + p_d)\,T_s$ | 0.785 | 0.785 | |
| Euler discrete pole, $\omega_n$ | $1 - \omega_n T_s$ | 0.686 | | |
| Exact discrete pole, $\omega_n$ | $e^{-\omega_n T_s}$ | 0.730 | | |
| Euler discrete pole, $p_d$ | $1 - p_d T_s$ | 0.843 | | |
| Exact discrete pole, $p_d$ | $e^{-p_d T_s}$ | 0.855 | | |

A 1° angle error moves $\hat T_d$ by 0.043 Nm in one tick. The
acceptance ramp of 50 rad/s² corresponds to a $T_d$ of
$J \cdot 50 = 0.008$ Nm, so $\hat T_d$ settles on it from a few
hundredths of a degree of sustained error, which is why that bullet asks
for $e$ within 0.01° once settled.

## 8. Symbols

| Symbol | Meaning | Unit | Where it is set |
| --- | --- | --- | --- |
| $\theta,\ \omega$ | Rotor mechanical angle and velocity | rad, rad/s | plant |
| $T_d$ | Unmodelled torque on the rotor, positive in the sense of positive $\omega$ | Nm | plant, estimated |
| $\tau$ | Commanded torque, $K_t\,i_q$ | Nm | motor-control application, `fw~mc_016~1` |
| $J$ | Rotor inertia | kg·m² | config, `fw~est_rotor_002~1` |
| $b$ | Viscous friction | Nm per rad/s | config, `fw~est_rotor_002~1` |
| $T_c$ | Coulomb friction, applied as $T_c\,\operatorname{sat}(\hat\omega/\omega_{db})$ | Nm | config, `fw~est_rotor_002~1` |
| $\omega_{db}$ | The speed at which the Coulomb ramp reaches $\pm T_c$ | rad/s | config, `fw~est_rotor_002~1` |
| $\theta_m$ | Encoder angle | rad | `fw~est_encoder_004~1` |
| $x,\ \hat x,\ e$ | State, its estimate, the estimation error $x - \hat x$ | mixed | |
| $v,\ u$ | Known input: $v = \tau - T_c\,\operatorname{sat}(\hat\omega/\omega_{db})$ in Nm; $u = (v - b\,\hat\omega + \hat T_d)/J$ in rad/s² | Nm, rad/s² | |
| $A,\ B,\ C$ | Continuous state, input and output matrices | | section 2 |
| $L,\ l_1 \ldots l_3$ | Continuous observer gains | rad/s, rad/s², Nm/rad | section 4 |
| $\omega_n,\ \zeta$ | Natural frequency and damping of the error pole pair | rad/s | config, `fw~est_rotor_001~1` |
| $p_d$ | Disturbance pole, the error dynamics' third pole | rad/s | config, `fw~est_rotor_001~1` |
| $T,\ T_{pwm}$ | PWM period, the prediction step | s (50 µs) | config, `fw~est_rotor_002~1` |
| $T_s$ | Encoder tick, the correction step | s (1 ms) | `fw~est_rotor_001~1` |
| $F,\ G$ | Discrete prediction matrices | | section 5 |
| $K$ | Discrete correction gains, $T_s L$ | | section 6 |

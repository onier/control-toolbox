/**********************************************************************************************************************
This file is part of the Control Toolbox (https://github.com/ethz-adrl/control-toolbox), copyright by ETH Zurich.
Licensed under the BSD-2 license (see LICENSE file in main directory)
**********************************************************************************************************************/

#pragma once

#include "SystemModelBase.h"

namespace ct {
namespace optcon {

/*!
 * \ingroup Filter
 *
 * \brief System model adapted to CT. System model encapsulates the integrator, so it is able to propagate the system,
 *        but also computes derivatives w.r.t. both state and noise. When propagating the system, CTSystemModel does not
 *        use the specified control input, but uses the assigned system controller instead.
 *
 * \todo   this needs to get unified with the system-interface from optcon
 *
 * @tparam STATE_DIM
 * @tparam CONTROL_DIM
 */
template <size_t STATE_DIM, size_t CONTROL_DIM, typename SCALAR = double>
class CTSystemModel final : public SystemModelBase<STATE_DIM, CONTROL_DIM, SCALAR>
{
public:
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    using Base = SystemModelBase<STATE_DIM, CONTROL_DIM, SCALAR>;
    using typename Base::control_vector_t;
    using typename Base::state_matrix_t;
    using typename Base::state_vector_t;
    using typename Base::Time_t;

    using SensitivityApprox_t =
        ct::core::SensitivityApproximation<STATE_DIM, CONTROL_DIM, STATE_DIM / 2, STATE_DIM / 2, SCALAR>;

    //! Constructor. Takes in the system with defined controller, and sens approximator for computing the derivatives
    CTSystemModel(std::shared_ptr<ct::core::ControlledSystem<STATE_DIM, CONTROL_DIM, SCALAR>> system,
        std::shared_ptr<SensitivityApprox_t> sensApprox,
        const state_matrix_t& dFdv,
        const ct::core::IntegrationType& intType = ct::core::IntegrationType::EULERCT);

    //! Propagates the system giving the next state as output. Control input is generated by the system controller.
    state_vector_t computeDynamics(const state_vector_t& state,
        const control_vector_t& u,
        const Time_t dt,
        Time_t t) override;

    //! Computes the derivative w.r.t state. Control input is generated by the system controller.
    state_matrix_t computeDerivativeState(const state_vector_t& state,
        const control_vector_t& u,
        const Time_t dt,
        Time_t t) override;

    //! Computes the derivative w.r.t noise. Control input is generated by the system controller.
    state_matrix_t computeDerivativeNoise(const state_vector_t& state,
        const control_vector_t& control,
        const Time_t dt,
        Time_t t) override;

protected:
    //! The underlying CT system.
    std::shared_ptr<ct::core::ControlledSystem<STATE_DIM, CONTROL_DIM, SCALAR>> system_;

    //! each system gets re-assigned a constant controller for dynamics evaluation with known control inputs
    std::shared_ptr<ct::core::ConstantController<STATE_DIM, CONTROL_DIM, SCALAR>> constantController_;

    //! The sensitivity approximator
    std::shared_ptr<SensitivityApprox_t> sensApprox_;

    //! Derivative w.r.t. noise.
    state_matrix_t dFdv_;

    //! Integrator.
    ct::core::Integrator<STATE_DIM, SCALAR> integrator_;
};

}  // namespace optcon
}  // namespace ct

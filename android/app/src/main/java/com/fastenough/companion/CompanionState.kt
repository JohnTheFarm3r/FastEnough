package com.fastenough.companion

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/** Live state shared between the service and the UI. */
object CompanionState {
    data class Address(val ip: String, val kind: String, val iface: String)

    data class Status(
        val serverRunning: Boolean = false,
        val serverError: String = "",
        val clients: Int = 0,
        val addresses: List<Address> = emptyList(),
        val rootActive: Boolean = false,
    )

    data class Approval(val clientId: String, val name: String)

    private val _status = MutableStateFlow(Status())
    val status: StateFlow<Status> = _status.asStateFlow()

    private val _approvals = MutableStateFlow<List<Approval>>(emptyList())
    val approvals: StateFlow<List<Approval>> = _approvals.asStateFlow()

    private val _trusted = MutableStateFlow<List<CompanionSettings.TrustedComputer>>(emptyList())
    val trusted: StateFlow<List<CompanionSettings.TrustedComputer>> = _trusted.asStateFlow()

    fun update(change: (Status) -> Status) {
        synchronized(this) { _status.value = change(_status.value) }
    }

    fun setApprovals(list: List<Approval>) {
        _approvals.value = list
    }

    fun refreshTrusted() {
        _trusted.value = CompanionSettings.trustedComputers()
    }
}

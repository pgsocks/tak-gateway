"use strict";

const $ = (selector, root = document) => root.querySelector(selector);
const state = { data: null, tab: "connections", editor: null,
	importJob: null, importInputs: [], importEpoch: 0, importSaving: false,
	importCommitted: false, remove: null, refreshing: false };
const clone = value => JSON.parse(JSON.stringify(value));
const node = (tag, text, className) => {
	const result = document.createElement(tag);
	if (text != null) result.textContent = String(text);
	if (className) result.className = className;
	return result;
};
const id = prefix => {
	const bytes = new Uint8Array(8);
	crypto.getRandomValues(bytes);
	return `${prefix}-${Array.from(bytes, value => value.toString(16).padStart(2, "0")).join("")}`;
};

function notice(message, kind = "info", target = "#notice") {
	const box = $(target);
	delete box.dataset.apply;
	delete box.dataset.offline;
	box.textContent = message || "";
	box.className = `notice ${kind}`;
	box.hidden = !message;
}

async function api(path, options = {}) {
	const headers = new Headers(options.headers || {});
	if (options.method && options.method !== "GET") {
		headers.set("X-TG-Token", state.data?.token || "");
		if (options.body && !(options.body instanceof File) &&
			!(options.body instanceof Blob)) {
			headers.set("Content-Type", "application/json");
			options.body = JSON.stringify(options.body);
		}
	}
	const response = await fetch(path, { ...options, headers,
		credentials: "same-origin", cache: "no-store" });
	let body;
	try { body = await response.json(); } catch (_) { body = {}; }
	if (!response.ok) {
		const field = body.error?.key || body.key;
		const error = new Error(response.status === 409 ?
			"The configuration changed in another window. Your form has not been changed. Close and reopen it to review the latest settings." :
			`${body.error?.message || body.message || `The gateway rejected this request (${response.status}).`}${field ? ` (${field})` : ""}`);
		error.status = response.status;
		error.key = body.error?.key || body.key;
		throw error;
	}
	return body;
}

async function waitForJob(result) {
	if (!result.job_id) return result;
	for (let attempt = 0; attempt < 180; attempt++) {
		await new Promise(resolve => setTimeout(resolve, 300));
		const job = await api(`/api/jobs/${encodeURIComponent(result.job_id)}`);
		if (job.state === "failed") throw new Error(
			job.error?.message || job.message || "The gateway could not apply this change.");
		if (["ready", "committed", "complete", "completed"].includes(job.state))
			return job;
	}
	throw new Error("The gateway is still processing this change. Refresh the page to check its status before submitting it again.");
}

function config() { return state.data?.config || { inputs: [], outputs: [] }; }
function endpoints(kind) { return config()[kind === "input" ? "inputs" : "outputs"] || []; }
function statusFor(kind, endpoint) {
	const list = state.data?.status?.[kind === "input" ? "inputs" : "outputs"] || [];
	return list.find(item => item.id === endpoint.id) || {};
}
function diagnostic(status) {
	return status.diagnostic || status.last_error || status.last_attempt?.diagnostic || {};
}
function failureText(status) {
	const error = diagnostic(status);
	return error.message || error.summary || status.message || status.failure || "";
}
function statusLabel(kind, endpoint, status) {
	if (endpoint.enabled === false) return ["Disabled", "neutral"];
	const phase = String(status.state || "DISCONNECTED").toUpperCase();
	const error = diagnostic(status);
	const code = String(error.code || "");
	if (code === "tls-client-certificate-rejected")
		return ["Authentication rejected", "bad"];
	if (code === "tls-access-denied") return ["Access denied", "bad"];
	if (code.startsWith("tls-server-")) return ["TLS verification failed", "bad"];
	if (/credential|password-load|ca-load|pkcs12-load|client-load|key-mismatch/.test(code))
		return ["Credential error", "bad"];
	if (kind === "output") {
		if (status.available) return ["Available", "good"];
		if (/FAILED|ERROR|BACKOFF/.test(phase) || failureText(status))
			return ["Output unavailable", "bad"];
		return [phase === "LISTENING" ? "Listening" : "Waiting", "neutral"];
	}
	if (phase === "XML_ACTIVE" || phase === "PROTOBUF_ACTIVE") {
		if (status.events_received > 0) return ["Receiving", "good"];
		if (status.protocol_offer_received) return ["Stream ready", "good"];
		return ["TLS connected", "good"];
	}
	if (phase === "LISTENING") return ["Listening", "good"];
	if (/FAILED|ERROR|BACKOFF/.test(phase)) return ["Connection failed", "bad"];
	const labels = { CONNECTING: "Connecting", TLS_HANDSHAKE: "TLS handshake",
		AUTH_PREAMBLE: "Sending credentials", UPGRADE_PENDING: "Negotiating",
		DISCONNECTED: "Waiting to connect", IDLE: "Waiting" };
	return [labels[phase] || phase.toLowerCase().replaceAll("_", " "), "warning"];
}
function badge(text, kind) { return node("span", text, `badge ${kind}`); }
function time(value) {
	if (!value) return "—";
	const date = new Date(value);
	return Number.isNaN(date.valueOf()) ? String(value) : date.toLocaleString();
}
function address(endpoint) {
	if (endpoint.kind === "pipe") return endpoint.path || "Platform pipe";
	const host = endpoint.host || endpoint.bind || "0.0.0.0";
	return `${host.includes(":") ? `[${host}]` : host}:${endpoint.port || "—"}`;
}
function action(label, run, extra = "") {
	const button = node("button", label, `link-button ${extra}`);
	button.addEventListener("click", run);
	return button;
}

function render() {
	if (!state.data) return;
	const inputs = endpoints("input"), outputs = endpoints("output");
	$("#revision").textContent = `Saved revision ${state.data.revision ?? "—"}`;
	if (state.data.apply_error || (state.data.active_revision != null &&
		state.data.active_revision !== state.data.revision)) {
		const message = state.data.apply_error?.message || "Waiting for the saved configuration to be applied.";
		notice(`Saved revision ${state.data.revision}; active revision ${state.data.active_revision ?? "—"}. ${message}`,
			state.data.apply_error ? "error" : "warning");
		$("#notice").dataset.apply = "true";
	} else if ($("#notice").dataset.apply === "true") {
		delete $("#notice").dataset.apply;
		notice("");
	}
	const processState = state.data.status?.state || "IDLE";
	const gatewayBadge = $("#gateway-state");
	gatewayBadge.textContent = processState === "RUNNING" ? "Gateway running" :
		processState === "IDLE" || processState === "READY" ? "Ready" : processState;
	gatewayBadge.className = `badge ${processState === "FAILED" ? "bad" : "good"}`;
	$("#connection-count").textContent = inputs.length;
	$("#output-count").textContent = outputs.length;
	const statuses = inputs.map(item => statusLabel("input", item, statusFor("input", item)));
	$("#connected-count").textContent = statuses.filter(item => item[1] === "good").length;
	$("#attention-count").textContent = statuses.filter(item => item[1] === "bad").length;
	renderRows("input", inputs, "#connection-rows", "#connections-empty");
	renderRows("output", outputs, "#output-rows", "#outputs-empty");
}

function renderRows(kind, items, target, empty) {
	const body = $(target);
	body.replaceChildren();
	$(empty).hidden = items.length !== 0;
	$(target).closest(".table-scroll").hidden = items.length === 0;
	for (const endpoint of items) {
		const status = statusFor(kind, endpoint);
		const row = node("tr");
		row.dataset.id = endpoint.id;
		const identity = node("td");
		identity.append(node("span", endpoint.name || endpoint.id, "endpoint-name"),
			node("span", address(endpoint), "endpoint-address"));
		const type = node("td");
		type.append(node("span", kind === "input" ?
			(endpoint.protocol || endpoint.encoding || endpoint.kind || "TLS") :
			(endpoint.kind === "tcp" ? `TCP ${endpoint.mode || "listen"}` : endpoint.kind?.toUpperCase()), ""));
		if (endpoint.framing) type.append(node("span", endpoint.framing, "minor"));
		const health = node("td");
		const label = statusLabel(kind, endpoint, status);
		health.append(badge(...label));
		const summary = failureText(status) ||
			(label[0] === "TLS connected" ? "Stream acceptance not yet observed" : "");
		if (summary) health.append(node("div", summary, "status-summary"));
		if (state.data.active_revision != null && state.data.active_revision !== state.data.revision)
			health.append(node("div", `Live status is from active revision ${state.data.active_revision}.`, "status-summary"));
		const activity = node("td");
		if (kind === "input") {
			activity.append(node("span", `${status.events_received || 0} events`),
				node("span", status.last_data_at_ms || status.last_activity_ms || status.last_event_at ?
					time(status.last_data_at_ms || status.last_activity_ms || status.last_event_at) :
					`${status.reconnects || 0} reconnects`, "minor"));
		} else {
			activity.append(node("span", `${status.queued_records || 0} queued`),
				node("span", `${status.clients || 0} consumers · ${status.records_dropped || 0} dropped`, "minor"));
		}
		const buttons = node("td"), actions = node("div", null, "row-actions");
		actions.append(action("Details", () => showDetails(kind, endpoint)),
			action("Edit", () => openEditor(kind, endpoint)),
			action(endpoint.enabled === false ? "Enable" : "Disable", () => toggle(kind, endpoint)),
			action("Remove", () => requestRemove(kind, endpoint), "destructive"));
		buttons.append(actions);
		row.append(identity, type, health, activity, buttons);
		body.append(row);
	}
}

async function refresh() {
	if (state.refreshing) return;
	state.refreshing = true;
	try {
		state.data = await api("/api/state");
		$("#refresh-status").textContent = `Updated ${new Date().toLocaleTimeString()}`;
		$("#refresh-status").classList.remove("error");
		if ($("#notice").dataset.offline === "true") {
			notice("");
			delete $("#notice").dataset.offline;
		}
		render();
	} catch (error) {
		$("#refresh-status").textContent = "Disconnected · showing last known status";
		$("#gateway-state").textContent = "Gateway unavailable";
		$("#gateway-state").className = "badge bad";
		notice("The management connection is unavailable. Displayed connection status may be out of date.", "warning");
		$("#notice").dataset.offline = "true";
	} finally { state.refreshing = false; }
}

function setTab(tab) {
	state.tab = tab;
	for (const name of ["connections", "outputs"]) {
		const selected = name === tab;
		$(`#${name}-tab`).classList.toggle("active", selected);
		$(`#${name}-tab`).setAttribute("aria-selected", String(selected));
		$(`#${name}-panel`).hidden = !selected;
	}
	$("#connection-actions").hidden = tab !== "connections";
	$("#output-actions").hidden = tab !== "outputs";
}

function field(container, key, label, options = {}) {
	const wrapper = node("label", null, `form-field ${options.wide ? "span-all" : ""}`);
	const inputId = `field-${key.replaceAll(".", "-")}`;
	wrapper.htmlFor = inputId;
	wrapper.append(node("span", label));
	let input;
	if (options.choices) {
		input = node("select");
		for (const [value, title] of options.choices) {
			const option = node("option", title); option.value = value; input.append(option);
		}
	} else {
		input = node("input"); input.type = options.type || "text";
		if (options.min != null) input.min = options.min;
		if (options.max != null) input.max = options.max;
		if (options.accept) input.accept = options.accept;
	}
	input.id = inputId; input.name = key;
	input.setAttribute("aria-label", label);
	input.required = Boolean(options.required);
	input.readOnly = Boolean(options.readOnly);
	if (input.type !== "file") input.value = state.editor.values[key] ?? options.value ?? "";
	if (options.placeholder) input.placeholder = options.placeholder;
	if (options.autocomplete) input.autocomplete = options.autocomplete;
	wrapper.append(input);
	const selected = state.editor.files[key];
	const help = selected ? `Selected: ${selected.name}` : options.help;
	if (help) wrapper.append(node("span", help, "field-help"));
	input.addEventListener("change", () => {
		if (input.type === "file") state.editor.files[key] = input.files[0];
		else state.editor.values[key] = input.value;
		if (options.rebuild) { collectFields(); paintEditor(); }
	});
	container.append(wrapper);
	return input;
}

function collectFields() {
	if (!state.editor) return;
	for (const input of $("#editor-fields").querySelectorAll("input,select,textarea")) {
		if (input.type === "file") {
			if (input.files[0]) state.editor.files[input.name] = input.files[0];
		} else if (input.type === "checkbox") state.editor.values[input.name] = input.checked;
		else state.editor.values[input.name] = input.value;
	}
}
function grid(parent) { const result = node("div", null, "form-grid"); parent.append(result); return result; }
function section(parent, title, description, advanced = false) {
	const result = node(advanced ? "details" : "section", null, "form-section");
	result.append(node(advanced ? "summary" : "h3", title));
	if (description) result.append(node("p", description, "section-description"));
	parent.append(result); return result;
}

function openEditor(kind, original = null) {
	if (!state.data) return notice("Wait for the gateway to load before editing.", "warning");
	const profile = original || { id: id(kind === "input" ? "tak" : "output"),
		kind: kind === "input" ? "tls" : "tcp", enabled: true };
	const values = {};
	function flatten(object, prefix = "") {
		for (const [key, value] of Object.entries(object)) {
			const path = prefix ? `${prefix}.${key}` : key;
			if (value && typeof value === "object" && !Array.isArray(value)) flatten(value, path);
			else values[path] = value;
		}
	}
	flatten(profile);
	if (profile.tls?.server_name === profile.host)
		values["tls.server_name"] = "";
	if (profile.callsign === profile.uid) values.callsign = "";
	values.identity_mode = profile.identity_mode ||
		(profile.tls?.has_pkcs12 || profile.tls?.pkcs12_file ? "pkcs12" :
			profile.tls?.has_certificate || profile.tls?.cert_file ? "pem" : "none");
	values.output_type = profile.kind === "tcp" && profile.mode === "connect" ? "tcp-connect" : profile.kind;
	state.editor = { kind, original, values, files: {}, revision: state.data.revision };
	$("#editor-title").textContent = `${original ? "Edit" : "Add"} ${kind === "input" ? "connection" : "output"}`;
	$("#editor-eyebrow").textContent = kind === "input" ? "TAK source" : "Event delivery";
	$("#editor-help").textContent = kind === "input" ?
		"Saving reconnects this source. Other connections and outputs keep running." :
		"Saving restarts this output and may reconnect its consumers. Other outputs keep running.";
	notice("", "error", "#form-error");
	paintEditor();
	$("#editor-dialog").showModal();
}

function paintEditor() {
	const editor = state.editor, parent = $("#editor-fields");
	parent.replaceChildren();
	const basic = grid(parent);
	field(basic, "name", "Display name", { placeholder: "Operations TAK Server" });
	field(basic, "id", editor.kind === "input" ? "Connection ID" : "Output ID", { required: true, readOnly: Boolean(editor.original), help: "Stable identifier used in configuration and status." });
	const enabledLabel = node("label", null, "checkbox-label");
	const enabled = node("input"); enabled.type = "checkbox"; enabled.name = "enabled";
	enabled.checked = editor.values.enabled !== false;
	enabledLabel.append(enabled, node("span", "Enabled")); parent.append(enabledLabel);
	if (editor.kind === "input") paintInput(parent); else paintOutput(parent);
}

function paintInput(parent) {
	const values = state.editor.values;
	const endpoint = grid(parent);
	field(endpoint, "kind", "Source type", { value: "tls", choices: [["tls", "TAK Server (verified TLS)"], ["udp", "UDP source"], ["multicast", "Multicast source"]], rebuild: true });
	const kind = values.kind || "tls";
	if (kind !== "tls") {
		field(endpoint, "bind", "Bind address", { value: "0.0.0.0" });
		field(endpoint, "port", "Port", { type: "number", min: 1, max: 65535, required: true });
		field(endpoint, "encoding", "Datagram encoding", { value: "auto", choices: [["auto", "Auto"], ["xml", "XML"], ["tak-v1", "TAK v1"]] });
		if (kind === "multicast") {
			field(endpoint, "group", "Multicast group", { required: true });
			field(endpoint, "interface", "Interface", { required: true });
		}
		return;
	}
	field(endpoint, "host", "Server hostname or IP", { required: true, placeholder: "tak.example.com" });
	field(endpoint, "port", "Streaming port", { type: "number", value: 8089, min: 1, max: 65535, required: true });
	field(endpoint, "uid", "Gateway UID", { value: values.id, required: true });
	field(endpoint, "callsign", "Callsign", { placeholder: "Defaults to gateway UID" });
	field(endpoint, "protocol", "Protocol", { value: "xml-only", choices: [["xml-only", "XML only"], ["negotiated", "Negotiate XML / Protobuf"], ["protobuf-only", "Protobuf v1 only"]], rebuild: true });
	if (values.protocol === "negotiated") field(endpoint, "prefer", "Preferred protocol", { value: "xml", choices: [["xml", "XML"], ["protobuf", "Protobuf"]] });
	const tls = section(parent, "Trust and client identity", "Server certificate and hostname verification remain enabled. Existing credentials are kept unless you replace them.");
	const credentials = grid(tls);
	field(credentials, "tls.server_name", "Certificate server name", { placeholder: "Defaults to server hostname" });
	field(credentials, "ca_upload", "CA trust certificate (PEM)", { type: "file", accept: ".pem,.crt,.cer", help: values["tls.has_ca"] ? "A CA trust certificate is already configured." : "Upload the CA that signs the TAK server certificate." });
	field(credentials, "identity_mode", "Client identity", { choices: [["none", "No client certificate"], ["pem", "PEM certificate and key"], ["pkcs12", "PKCS#12 certificate bundle"]], rebuild: true });
	if (values.identity_mode === "pem") {
		field(credentials, "certificate_upload", "Client certificate (PEM)", { type: "file", accept: ".pem,.crt,.cer", help: values["tls.has_certificate"] ? "A client certificate is configured." : "" });
		field(credentials, "key_upload", "Client private key (PEM)", { type: "file", accept: ".pem,.key", help: values["tls.has_key"] ? "A private key is configured; its contents are never shown." : "" });
		field(credentials, "tls.key_password", "New private-key passphrase", { type: "password", autocomplete: "new-password", help: "Leave blank to keep the existing passphrase." });
	} else if (values.identity_mode === "pkcs12") {
		field(credentials, "pkcs12_upload", "PKCS#12 bundle", { type: "file", accept: ".p12,.pfx", help: values["tls.has_pkcs12"] ? "A PKCS#12 identity is configured." : "" });
		field(credentials, "tls.pkcs12_password", "New bundle password", { type: "password", autocomplete: "new-password", help: "Leave blank to keep the existing password." });
	}
	const auth = section(parent, "Streaming authentication", "Streaming username/password credentials are separate from certificate authentication and enrollment.");
	const authentication = grid(auth);
	field(authentication, "auth.mode", "Authentication", { value: "none", choices: [["none", "Certificate / no password preamble"], ["password", "Username and password"]], rebuild: true });
	if (values["auth.mode"] === "password") {
		field(authentication, "auth.username", "Username", { required: true, autocomplete: "username" });
		field(authentication, "auth.password", "New streaming password", { type: "password", autocomplete: "new-password", help: values["auth.has_password"] ? "A password is configured. Leave blank to keep it." : "The password is stored privately and is never shown again." });
	}
	const advanced = section(parent, "Advanced settings", "Use file or environment references for existing server-side credentials. Blank references keep their current values.", true);
	const advancedGrid = grid(advanced);
	field(advancedGrid, "tls.ca_file", "CA file on gateway host");
	if (values.identity_mode === "pem") {
		field(advancedGrid, "tls.cert_file", "Client certificate file");
		field(advancedGrid, "tls.key_file", "Private-key file");
		field(advancedGrid, "tls.key_password_env", "Private-key passphrase environment variable");
	} else if (values.identity_mode === "pkcs12") {
		field(advancedGrid, "tls.pkcs12_file", "PKCS#12 file on gateway host");
		field(advancedGrid, "tls.pkcs12_password_env", "Bundle password environment variable");
	}
	if (values["auth.mode"] === "password") field(advancedGrid, "auth.password_env", "Streaming password environment variable");
	field(advancedGrid, "maintenance.ping_interval_ms", "Ping interval (ms)", { type: "number", min: 0, value: 0 });
	field(advancedGrid, "maintenance.ping_timeout_ms", "Ping timeout (ms)", { type: "number", min: 0, value: 0 });
	const clearLabel = node("label", null, "checkbox-label");
	const clear = node("input"); clear.type = "checkbox"; clear.name = "clear_passwords";
	clear.checked = values.clear_passwords === true;
	clearLabel.append(clear, node("span", "Remove stored passphrases and streaming password before applying new values"));
	advanced.append(clearLabel);
}

function paintOutput(parent) {
	const values = state.editor.values, fields = grid(parent);
	field(fields, "output_type", "Output type", { value: "tcp", choices: [["tcp", "TCP listener"], ["tcp-connect", "TCP outbound connection"], ["udp", "UDP destination"], ["pipe", "Platform pipe"]], rebuild: true });
	const type = values.output_type || "tcp";
	if (type === "pipe") {
		field(fields, "path", "Pipe path", { required: true, placeholder: "/tmp/tak-events" });
		field(fields, "permissions", "POSIX permissions", { value: "0600", help: "Private mode; no access for other users." });
		const label = node("label", null, "checkbox-label span-all");
		const create = node("input"); create.type = "checkbox"; create.name = "create";
		create.checked = values.create !== false;
		label.append(create, node("span", "Create pipe if missing")); fields.append(label);
	} else {
		field(fields, type === "tcp" ? "bind" : "host", type === "tcp" ? "Bind address" : "Destination host", { value: "127.0.0.1", required: true });
		field(fields, "port", "Port", { type: "number", min: 1, max: 65535, value: 9000, required: true });
	}
	if (type === "udp") field(fields, "max_datagram_bytes", "Maximum datagram bytes", { type: "number", min: 1, max: 65507, value: 65507 });
	else field(fields, "framing", "Stream framing", { value: "u32be", choices: [["u32be", "Length-prefixed XML (u32be)"], ["xml-document", "XML documents"]] });
	if (type === "tcp") field(fields, "max_clients", "Maximum clients", { type: "number", min: 1, value: 32 });
	const advanced = section(parent, "Output queue", "Leave limits blank to use the gateway defaults. Delivery remains live and best effort.");
	const queue = grid(advanced);
	field(queue, "queue_bytes", "Queue bytes", { type: "number", min: 1, placeholder: "Gateway default" });
	field(queue, "queue_records", "Queue records", { type: "number", min: 1, placeholder: "Gateway default" });
}

function payload() {
	collectFields();
	const editor = state.editor, values = editor.values;
	const result = { id: values.id.trim(), enabled: values.enabled !== false };
	result.name = values.name?.trim() || null;
	const copy = (names, target = result, prefix = "") => {
		for (const name of names) {
			const value = values[`${prefix}${name}`];
			if (value !== undefined && value !== "") target[name] = value;
		}
	};
	const number = (names, target = result, prefix = "") => {
		for (const name of names) {
			const value = values[`${prefix}${name}`];
			if (value !== undefined && value !== "") target[name] = Number(value);
		}
	};
	if (editor.kind === "input") {
		result.kind = values.kind || "tls";
		number(["port"]);
		if (result.kind !== "tls") {
			copy(["bind", "encoding"]);
			if (result.kind === "multicast") copy(["group", "interface"]);
			return result;
		}
		copy(["host", "uid", "callsign", "protocol"]);
		result.callsign = values.callsign || null;
		if (result.protocol === "negotiated") copy(["prefer"]);
		if (result.protocol === "protobuf-only") result.version = 1;
		result.identity_mode = values.identity_mode || "none";
		result.tls = {};
		copy(["server_name", "ca_file"], result.tls, "tls.");
		result.tls.server_name = values["tls.server_name"] || null;
		if (result.identity_mode === "pem") copy(["cert_file", "key_file", "key_password", "key_password_env"], result.tls, "tls.");
		if (result.identity_mode === "pkcs12") copy(["pkcs12_file", "pkcs12_password", "pkcs12_password_env"], result.tls, "tls.");
		result.auth = { mode: values["auth.mode"] || "none" };
		if (result.auth.mode === "password") copy(["username", "password", "password_env"], result.auth, "auth.");
		if (values.clear_passwords) {
			result.tls.clear_secrets = true; result.auth.clear_secrets = true;
		}
		const maintenance = {};
		number(["ping_interval_ms", "ping_timeout_ms"], maintenance, "maintenance.");
		for (const key of ["ping_interval_ms", "ping_timeout_ms"])
			if (values[`maintenance.${key}`] === "") maintenance[key] = null;
		if (Object.keys(maintenance).length) result.maintenance = maintenance;
	} else {
		const type = values.output_type || "tcp";
		result.kind = type === "tcp-connect" ? "tcp" : type;
		if (type === "pipe") {
			copy(["path", "permissions"]); result.permissions = values.permissions || null;
			result.create = values.create !== false;
		}
		else {
			number(["port"]);
			copy([type === "tcp" ? "bind" : "host"]);
			if (type === "tcp-connect") result.mode = "connect";
			if (type === "tcp") {
				result.mode = "listen"; number(["max_clients"]);
				if (values.max_clients === "") result.max_clients = null;
			}
		}
		if (type === "udp") number(["max_datagram_bytes"]); else copy(["framing"]);
		number(["queue_bytes", "queue_records"]);
		for (const key of ["queue_bytes", "queue_records"])
			if (!values[key]) result[key] = null;
		if (type === "udp" && values.max_datagram_bytes === "")
			result.max_datagram_bytes = null;
	}
	return result;
}

async function uploadAsset(file, kind) {
	if (file.size > 8 * 1024 * 1024) throw new Error("Credential files must be no larger than 8 MiB.");
	const asset = await api(`/api/assets?kind=${kind}`, { method: "POST", body: file,
		headers: { "Content-Type": "application/octet-stream" } });
	if (!asset.asset_id) throw new Error("The gateway did not return an uploaded credential reference.");
	return asset.asset_id;
}

async function saveEndpoint(event) {
	event.preventDefault();
	const button = $("#save-endpoint"); button.disabled = true;
	button.textContent = "Saving…";
	state.editor.saving = true;
	for (const close of document.querySelectorAll(".close-dialog")) close.disabled = true;
	notice("", "error", "#form-error");
	try {
		const input = payload(), editor = state.editor;
		if (editor.kind === "input" && input.kind === "tls") {
			const uploads = [["ca_upload", "ca", "ca_asset"],
				["certificate_upload", "certificate", "certificate_asset"],
				["key_upload", "key", "key_asset"], ["pkcs12_upload", "pkcs12", "pkcs12_asset"]];
			for (const [key, type, fieldName] of uploads) {
				if (editor.files[key] && (type === "ca" ||
					(input.identity_mode === "pem" && ["key", "certificate"].includes(type)) ||
					(input.identity_mode === "pkcs12" && type === "pkcs12")))
					input.tls[fieldName] = await uploadAsset(editor.files[key], type);
			}
		}
		const plural = editor.kind === "input" ? "inputs" : "outputs";
		const path = `/api/${plural}`;
		await waitForJob(await api(path, { method: editor.original ? "PUT" : "POST",
			body: { revision: editor.revision, id: editor.original?.id,
				[editor.kind]: input } }));
		$("#editor-dialog").close(); state.editor = null;
		notice("Configuration saved. The affected endpoint is applying its settings.");
		await refresh();
	} catch (error) { notice(error.message, "error", "#form-error"); }
	finally {
		button.disabled = false; button.textContent = "Save and apply";
		if (state.editor) state.editor.saving = false;
		for (const close of document.querySelectorAll(".close-dialog")) close.disabled = false;
	}
}

async function toggle(kind, endpoint) {
	try {
		const updated = { id: endpoint.id, enabled: endpoint.enabled === false };
		await waitForJob(await api(`/api/${kind === "input" ? "inputs" : "outputs"}`,
			{ method: "PUT", body: { revision: state.data.revision,
				id: endpoint.id, [kind]: updated } }));
		notice(`${endpoint.name || endpoint.id} ${updated.enabled ? "enabled" : "disabled"}.`);
		await refresh();
	} catch (error) { notice(error.message, "error"); }
}

function requestRemove(kind, endpoint) {
	state.remove = { kind, endpoint };
	$("#confirm-title").textContent = `Remove ${kind === "input" ? "connection" : "output"}?`;
	$("#confirm-description").textContent = `Remove “${endpoint.name || endpoint.id}” from the saved configuration? Other endpoints will keep running.`;
	$("#confirm-dialog").showModal();
}
async function removeEndpoint() {
	const pending = state.remove;
	$("#confirm-dialog").close(); state.remove = null;
	if (!pending) return;
	try {
		await waitForJob(await api(`/api/${pending.kind === "input" ? "inputs" : "outputs"}`,
			{ method: "DELETE", body: { revision: state.data.revision,
				id: pending.endpoint.id } }));
		notice("Endpoint removed from the saved configuration."); await refresh();
	} catch (error) { notice(error.message, "error"); }
}

function showDetails(kind, endpoint) {
	const status = statusFor(kind, endpoint), content = $("#details-content");
	content.replaceChildren();
	$("#details-title").textContent = endpoint.name || endpoint.id;
	content.append(badge(...statusLabel(kind, endpoint, status)));
	if (state.data.active_revision != null && state.data.active_revision !== state.data.revision)
		content.append(node("div", `The endpoint fields below are saved settings. Live status still belongs to revision ${state.data.active_revision}; saved revision ${state.data.revision} has not been applied.`, "notice warning"));
	const error = diagnostic(status), info = node("dl", null, "details-grid");
	const facts = kind === "output" ? { Endpoint: address(endpoint),
		State: status.state || "Waiting", Available: status.available ? "Yes" : "No",
		Consumers: status.clients || 0, "Queued records": status.queued_records || 0,
		"Queued bytes": status.queued_bytes || 0,
		"Records completed": status.records_completed || 0,
		"Records dropped": status.records_dropped || 0 } :
		{ Endpoint: address(endpoint), State: status.state || "Waiting",
		"Last activity": time(status.last_data_at_ms || status.last_activity_ms || status.last_event_at),
		"Reconnect attempts": status.reconnects || 0,
		"Events received": status.events_received || 0,
		"Credentials sent": status.credentials_sent || error.credentials_sent ? "Yes — this alone does not confirm acceptance" : "No",
		"Server protocol offer": status.protocol_offer_received || error.protocol_offer_received ? "Received" : "Not observed",
		"Retry deadline": time(status.retry_at_ms || status.next_retry_at || error.retry_at_ms),
		"Attempt": status.attempt_id || error.attempt || status.last_attempt?.id || "—",
		"Profile revision": error.profile_revision || "—" };
	$("#details-dialog .eyebrow").textContent = kind === "output" ? "Output details" : "Connection details";
	for (const [label, value] of Object.entries(facts)) info.append(node("dt", label), node("dd", value));
	content.append(info);
	const explanation = failureText(status);
	if (explanation) {
		const block = node("section", null, "details-block");
		block.append(node("h3", "Latest diagnostic"), node("p", explanation));
		const details = node("dl", null, "details-grid");
		for (const key of ["category", "stage", "code", "native_domain", "native_code", "native_message", "ssl_error", "tls_verify_result", "tls_alert"]) {
			if (error[key] !== undefined && error[key] !== "") details.append(node("dt", key.replaceAll("_", " ")), node("dd", error[key]));
		}
		block.append(details); content.append(block);
	}
	if ((status.credentials_sent || error.credentials_sent) &&
		!status.protocol_offer_received && !error.protocol_offer_received &&
		!status.events_received && !error.events_received) {
		const note = node("div", "Sending credentials is not proof of authentication. A server may close a stream without returning a rejection reason.", "notice warning");
		content.append(note);
	}
	const history = status.history || status.attempts || [];
	if (history.length) {
		content.append(node("h3", "Recent attempts"));
		const list = node("ol", null, "history-list");
		for (const attempt of history.slice(-20).reverse()) {
			const item = node("li");
			item.append(node("strong", time(attempt.failed_at_ms || attempt.at || attempt.started_at_ms)),
				node("div", attempt.message || attempt.diagnostic?.message || attempt.failure || attempt.state || "Attempt recorded"));
			list.append(item);
		}
		content.append(list);
	}
	$("#details-dialog").showModal();
}

function openImport() {
	if (!state.data) return notice("Wait for the gateway to load before importing.", "warning");
	state.importJob = null; state.importInputs = [];
	state.importCommitted = false; state.importEpoch++;
	$("#zip-file").value = ""; $("#import-preview").replaceChildren();
	$("#import-preview").hidden = true; $("#import-progress").hidden = true;
	$("#confirm-import").disabled = true; notice("", "error", "#import-error");
	$("#import-dialog").showModal();
}
async function importZip() {
	const file = $("#zip-file").files[0];
	if (!file) return;
	const epoch = ++state.importEpoch;
	const previous = state.importJob;
	state.importJob = null;
	if (previous) cancelPreview(previous);
	notice("", "error", "#import-error");
	$("#confirm-import").disabled = true;
	$("#import-progress").hidden = false;
	$("#import-progress").textContent = "Inspecting the package and certificate stores…";
	try {
		if (file.size > 16 * 1024 * 1024) throw new Error("The configuration ZIP must be no larger than 16 MiB.");
		const result = await api("/api/import", { method: "POST", body: file,
			headers: { "Content-Type": "application/zip" } });
		const job = result.job_id || result.id;
		if (epoch === state.importEpoch) state.importJob = job;
		const ready = result.preview ? result : await waitForJob(result);
		if (epoch !== state.importEpoch || !$("#import-dialog").open) {
			cancelPreview(job);
			return;
		}
		const preview = ready.preview || ready;
		state.importInputs = clone(preview.inputs || []);
		state.importRevision = state.data.revision;
		if (!state.importInputs.length) throw new Error("No importable TAK connections were found in this package.");
		const area = $("#import-preview"); area.replaceChildren(); area.hidden = false;
		if (preview.warnings?.length) area.append(node("div", preview.warnings.join(" "), "notice warning"));
		state.importInputs.forEach((input, index) => {
			const card = node("section", null, "preview-card");
			card.append(node("h3", `Connection ${index + 1}`));
			const fields = node("div", null, "form-grid");
			for (const [key, label, type] of [["name", "Display name", "text"], ["host", "Server", "text"], ["port", "Port", "number"], ["uid", "Gateway UID", "text"]]) {
				const wrapper = node("label", null, "form-field"); wrapper.append(node("span", label));
				const field = node("input"); field.type = type; field.value = input[key] || (key === "name" ? input.id : "");
				field.setAttribute("aria-label", `${label} for imported connection ${index + 1}`);
				if (type === "number") { field.min = 1; field.max = 65535; }
				field.addEventListener("input", () => {
					state.importInputs[index][key] = type === "number" ?
						Number(field.value) : key === "name" ?
						field.value.trim() || null : field.value;
				});
				wrapper.append(field); fields.append(wrapper);
			}
			card.append(fields); area.append(card);
		});
		$("#import-progress").textContent = `${state.importInputs.length} connection${state.importInputs.length === 1 ? "" : "s"} ready to review.`;
		$("#confirm-import").disabled = false;
	} catch (error) {
		if (epoch !== state.importEpoch || !$("#import-dialog").open) return;
		$("#import-progress").hidden = true; notice(error.message, "error", "#import-error");
	}
}
function cancelPreview(job) {
	return api(`/api/jobs/${encodeURIComponent(job)}`, { method: "DELETE" }).catch(() => {});
}
async function confirmImport() {
	const button = $("#confirm-import"); button.disabled = true;
	state.importSaving = true;
	for (const close of document.querySelectorAll('[data-close="import-dialog"]')) close.disabled = true;
	try {
		for (const [index, input] of state.importInputs.entries()) {
			if (!input.host?.trim() || !input.uid?.trim() ||
				!Number.isInteger(input.port) || input.port < 1 || input.port > 65535)
				throw new Error(`Connection ${index + 1} needs a server, gateway UID, and a port between 1 and 65535.`);
		}
		await waitForJob(await api(`/api/import/${encodeURIComponent(state.importJob)}/confirm`,
			{ method: "POST", body: { revision: state.importRevision, inputs: state.importInputs } }));
		state.importCommitted = true;
		$("#import-dialog").close(); notice("Imported connections saved. Connection attempts are starting.");
		await refresh();
	} catch (error) { notice(error.message, "error", "#import-error"); }
	finally {
		button.disabled = false; state.importSaving = false;
		for (const close of document.querySelectorAll('[data-close="import-dialog"]')) close.disabled = false;
	}
}

$("#connections-tab").addEventListener("click", () => setTab("connections"));
$("#outputs-tab").addEventListener("click", () => setTab("outputs"));
$("#add-connection").addEventListener("click", () => openEditor("input"));
$("#add-output").addEventListener("click", () => openEditor("output"));
$("#import-zip").addEventListener("click", openImport);
$("#zip-file").addEventListener("change", importZip);
$("#confirm-import").addEventListener("click", confirmImport);
$("#endpoint-form").addEventListener("submit", saveEndpoint);
$("#confirm-remove").addEventListener("click", removeEndpoint);
$("#cancel-remove").addEventListener("click", () => $("#confirm-dialog").close());
for (const button of document.querySelectorAll(".close-dialog")) button.addEventListener("click", () => $("#editor-dialog").close());
for (const button of document.querySelectorAll("[data-close]")) button.addEventListener("click", () => $(`#${button.dataset.close}`).close());
for (const button of document.querySelectorAll("[data-action]")) button.addEventListener("click", () => openEditor(button.dataset.action === "add-output" ? "output" : "input"));
$("#editor-dialog").addEventListener("close", () => {
	state.editor = null;
	$("#editor-fields").replaceChildren();
});
$("#editor-dialog").addEventListener("cancel", event => {
	if (state.editor?.saving) event.preventDefault();
});
$("#import-dialog").addEventListener("cancel", event => {
	if (state.importSaving) event.preventDefault();
});
$("#import-dialog").addEventListener("close", () => {
	state.importEpoch++;
	if (state.importJob && !state.importCommitted) cancelPreview(state.importJob);
	state.importJob = null; state.importInputs = [];
	$("#zip-file").value = "";
});
refresh();
setInterval(refresh, 1000);

Feature: Local network adapters compose the gateway pipeline
  The foreground runtime delivers real UDP input to downstream TCP consumers.

  Scenario: Preserve complete events through actual local sockets
    Given a runtime with a loopback UDP source and a TCP listener
    And a downstream TCP consumer is connected
    When an XML event with custom fields is sent by UDP
    Then the TCP consumer receives its exact u32be XML record

  Scenario: Reject a malformed datagram without disrupting subsequent traffic
    Given a runtime with a loopback UDP source and a TCP listener
    And a downstream TCP consumer is connected
    When a malformed UDP document is followed by a valid event
    Then the TCP consumer receives only the valid complete record

  Scenario: Status remains available when sources are quiet
    Given a runtime with a loopback UDP source and a TCP listener
    When local status is read without sending an event
    Then the snapshot identifies the running source and available output
    And the snapshot contains no event payload

  Scenario: Stop closes intake and preserves an inspectable final state
    Given a runtime with a loopback UDP source and a TCP listener
    When an immediate stop is requested twice
    Then the runtime reaches STOPPED and polling remains harmless

Feature: Loss-aware XML normalization
  Output remains XML while preserving information that ordinary mappings omit.

  Scenario: Preserve received XML bytes
    Given a complete XML event with unknown namespaced fields and CDATA
    When the event is normalized in preserve-opaque mode
    Then one event contains the exact original XML bytes

  Scenario: Consume a control-only Protobuf message
    Given a control-only Protobuf envelope
    When the envelope is normalized in preserve-opaque mode
    Then no application event is returned

  Scenario: Retain an event alongside Protobuf control information
    Given a Protobuf envelope containing control information and an event
    When the envelope is normalized in preserve-opaque mode
    Then one XML event has identifier "fixture-1"
    And no TAK control bytes appear in the preservation extension

  Scenario Outline: Preserve unmapped wire content exactly
    Given an event envelope with <unmapped>
    When the envelope is normalized in preserve-opaque mode
    Then one XML event has identifier "fixture-1"
    And the preservation extension contains the exact event-only envelope

    Examples:
      | unmapped                    |
      | an unknown event field      |
      | an unknown nested field     |
      | top-level server timestamps |

  Scenario: Reject incomplete standard mappings on request
    Given an event envelope with an unknown nested field
    When the envelope is normalized in reject mode
    Then conversion is rejected without partial XML

  Scenario: Keep unrelated application control-looking types
    Given an XML application event whose type starts with "t-"
    When the event is normalized in preserve-opaque mode
    Then one event contains the exact original XML bytes

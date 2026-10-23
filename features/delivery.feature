Feature: Bounded independent output delivery
  Every accepting output observes the same complete event order.

  Scenario: Preserve duplicates and admission order
    Given two healthy outputs with u32be framing
    When two identical XML events are published
    Then both outputs expose both complete records in admission order

  Scenario: A full output does not suppress a healthy output
    Given one full output and one healthy output
    When a new XML event is published with drop-newest policy
    Then the full output reports a local drop
    And the healthy output exposes the new complete record

  Scenario: Short writes never interleave records
    Given one healthy output with u32be framing
    When two XML events are published and consumed one byte at a time
    Then the collected stream contains two exact length-prefixed XML records

  Scenario: Output framing contains no separator or terminator
    Given one healthy output with u32be framing
    When one XML event is published
    Then its record contains a four-byte big-endian byte length
    And the record ends immediately after the XML document

  Scenario: UDP oversize affects only the UDP destination
    Given a UDP output whose payload limit is smaller than the XML event
    And a healthy stream output
    When one XML event is published
    Then the UDP output reports oversize without partial delivery
    And the healthy output exposes the new complete record

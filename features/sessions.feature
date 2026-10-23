Feature: Independent TAK session state
  Authentication and encoding changes occur independently for each server.

  Scenario: Forward XML without requesting an upgrade by default
    Given a negotiated session preferring XML
    When a supported-version announcement is received
    Then no upgrade request is queued
    And the session remains in XML mode

  Scenario: Switch encoding at the exact response boundary
    Given a negotiated session preferring Protobuf
    When a supported-version announcement is received
    And the complete upgrade request has been written
    And acceptance and a binary event arrive in the same read
    Then one XML event has identifier "fixture-1"
    And the session is in Protobuf mode

  Scenario: Continue receiving XML during negotiation
    Given a negotiated session preferring Protobuf
    When a supported-version announcement is received
    And the complete upgrade request has been written
    And an ordinary XML event arrives before acceptance
    Then one event contains the exact original XML bytes

  Scenario: Return to XML after explicit rejection
    Given a negotiated session preferring Protobuf
    When a supported-version announcement is received
    And the complete upgrade request has been written
    And the server rejects the upgrade
    Then the session remains in XML mode

  Scenario: Close after an indeterminate encoding timeout
    Given a negotiated session preferring Protobuf
    When a supported-version announcement is received
    And the complete upgrade request has been written
    And the response deadline expires
    Then the session requires a connection close

  Scenario: Credentials precede a negotiation request
    Given a password-authenticated session preferring Protobuf
    When a supported-version announcement is received
    Then the first upstream document contains the credential preamble
    And the credential preamble is never emitted as an application event

  Scenario: Reset partial state on reconnect
    Given a negotiated session preferring XML
    When part of an XML event arrives before a reconnect
    And a fresh complete XML event arrives after reset
    Then one event contains the exact original XML bytes

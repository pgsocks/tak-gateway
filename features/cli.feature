Feature: Configure gateway delivery from command line arguments
  A configuration file is optional when arguments describe the inputs,
  outputs, and gateway settings needed for the run.

  Scenario: Prepare container first boot without a server configuration
    Given no saved gateway configuration for container first boot
    When the container settings enable management and local TCP delivery
    Then management is enabled on all interfaces at port 8080
    And a local TCP output is configured at port 9000 with no inputs

  Scenario: Prepare a certificate input and TCP output without TOML
    Given an ATAK zip and no local TOML configuration
    When arguments select a TCP output and bounded delivery queues
    Then the composed gateway can prepare its TLS source and TCP output
    And its queue limit and overflow policy match the arguments

  Scenario: Override a saved output while preserving the saved input
    Given a saved UDP gateway configuration
    When arguments replace its output and select XML document framing
    Then its saved input and replacement output are both retained

  Scenario: Reject invalid settings after importing certificates
    Given an ATAK zip and no local TOML configuration
    When arguments select an invalid imported protocol
    Then command line configuration fails without publishing resources

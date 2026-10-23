Feature: Configuration validation before connecting
  Invalid settings fail before the gateway opens network endpoints.

  Scenario: Accept a minimal supported configuration
    Given a configuration with a UDP input and TCP listener output
    When the configuration is parsed
    Then the configuration is accepted

  Scenario Outline: Reject unsupported or ambiguous configuration
    Given a configuration with <defect>
    When the configuration is parsed
    Then the configuration is rejected with a key-specific diagnostic

    Examples:
      | defect                        |
      | an unknown top-level key      |
      | a zero input port             |
      | an unsupported ingress mode   |
      | duplicate input identifiers   |
      | an impossible queue limit     |
      | an obvious input-output loop  |

  Scenario: Redact secret references and credentials
    Given a configuration with password-file authentication
    When its resolved redacted representation is requested
    Then the representation contains no password or private-key material

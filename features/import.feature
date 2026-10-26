Feature: Import ATAK server configuration packages
  A server package supplies a verified TLS input while local configuration
  continues to define downstream delivery.

  Scenario: Use a wrapped ATAK mission package without manual conversion
    Given an ATAK package with a client certificate and trusted CA
    When its imported input is composed with a local TCP output
    Then the gateway can prepare the imported TLS source
    And no package password appears in its configuration or status

  Scenario: Use the separate client identity in a transport zip
    Given an enrollment package wrapped with one supplied client identity
    When its imported input is composed with a local TCP output
    Then the gateway can prepare the imported TLS source

  Scenario: Refuse an enrollment package without client credentials
    Given an enrollment package without a supplied client identity
    When the ATAK package import is attempted
    Then it fails without returning configuration or credential files

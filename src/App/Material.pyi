# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from Base.Metadata import export, class_declarations
from Base.PyObjectBase import PyObjectBase
from typing import Any, overload


@export(
    Constructor=True,
    Delete=True,
    RichCompare=True,
)
@class_declarations("""public:
    static Base::Color toColor(PyObject* value);
        """)
class Material(PyObjectBase):
    """
    App.Material class.

    Author: Werner Mayer (wmayer@users.sourceforge.net)
    Licence: LGPL
    UserDocu: This is the Material class
    """

    def set(self, string: str, /) -> None:
        """
        Set(string) -- Set the material.

        The material must be one of the following values:
        Brass, Bronze, Copper, Gold, Pewter, Plaster, Plastic, Silver, Steel, Stone, Shiny plastic,
        Satin, Metalized, Neon GNC, Chrome, Aluminium, Obsidian, Neon PHC, Jade, Ruby or Emerald.
        """
        ...

    AmbientColor: Any = ...
    """Ambient color"""

    DiffuseColor: Any = ...
    """Diffuse color"""

    EmissiveColor: Any = ...
    """Emissive color"""

    SpecularColor: Any = ...
    """Specular color"""

    Shininess: float = 0.0
    """Shininess"""

    Transparency: float = 0.0
    """Transparency"""

    Metallic: float = 0.0
    """Metallic amount (0 = dielectric, 1 = conductor) of a physical material"""

    Roughness: float = 0.5
    """Roughness (0 = mirror, 1 = fully diffuse) of a physical material"""

    UsePhysicalMaterial: bool = False
    """Enable the physically based metallic-roughness appearance"""

    TextureSize: float = 100.0
    """Real-world size (mm) spanned by one tile of the embedded texture image"""

    TextureMapping: str = "Planar"
    """Texture projection: 'Planar', 'Box', 'Spherical' or 'Cylindrical'"""

    Image: str = ""
    """Base64-encoded embedded texture image (PNG)"""

    ImagePath: str = ""
    """Path to an external texture image file"""

    RoughnessImage: str = ""
    """Base64-encoded roughness map (PNG)"""

    RoughnessImagePath: str = ""
    """Path to an external roughness map image file"""

    NormalImage: str = ""
    """Base64-encoded normal map (PNG)"""

    NormalImagePath: str = ""
    """Path to an external normal map image file"""

    EmissiveImage: str = ""
    """Base64-encoded emissive map (PNG)"""

    EmissiveImagePath: str = ""
    """Path to an external emissive map image file"""

    RoughnessStrength: float = 1.0
    """Multiplies the sampled roughness-map value"""

    NormalStrength: float = 1.0
    """Scales the tangent-space normal-map perturbation"""

    EmissiveIntensity: float = 1.0
    """Scales the emissive-map contribution"""

    TransmissionIor: float = 1.5
    """Index of refraction of the transmissive (glass) material"""

    TransmissionAbsorption: float = 0.0
    """Beer-Lambert absorption strength of the transmissive (glass) material"""
